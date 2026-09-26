// SPDX-License-Identifier: MIT
#include "consoleinterrupthandler.hpp"

#include "iocontext.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::atomic<std::stop_source*> g_source{nullptr};

// Test global set by `ConsoleInterruptHandlerTestUtil`. When
// it holds a value, the next construction throws as though the handler could
// not be installed. Test-only, so no need to be atomic.
std::optional<int> g_forced_install_error;

// The self-pipe between the signal handler and the watch on its read end. The
// handler touches only the write end, and only via write(2), which is
// async-signal-safe; the watch owns the read end.
//
// Created once and never closed: sigaction() does not wait for a handler that
// is already running, so a closed write end could be reopened as something
// else and catch that handler's byte. Two descriptors is the price of that.
std::atomic<int> g_wakeup_fd{-1};
int g_read_fd = -1;
std::once_flag g_pipe_created;

// What makes g_wakeup_fd legal to touch from a signal handler.
static_assert(std::atomic<int>::is_always_lock_free);

// What the handler sends through the pipe.
constexpr char k_interrupt = 'I';

// Previous dispositions, restored on teardown so a signal arriving afterwards
// takes the default action instead of running a handler for a stop source that
// no longer exists.
struct sigaction g_prev_int {};
struct sigaction g_prev_term {};

// Runs in signal context, so it writes to the self-pipe and lets the watch on
// the other end do the rest. request_stop() takes a lock and runs callbacks and
// so is *not* legal here, which is the whole reason for the pipe.
extern "C" void handle_signal(int /*signal*/) {
  // write(2) sets errno on failure, and this runs on whichever thread the
  // kernel picks, at whatever point that thread had reached. Left unrestored,
  // a failure here would be read as the errno of whatever syscall the
  // interrupted thread had just made.
  const int saved_errno = errno;

  const int fd = g_wakeup_fd.load(std::memory_order_relaxed);
  if (fd >= 0) {
    const char byte = k_interrupt;
    // A dropped byte would at worst miss one interrupt, and write(2) into a
    // pipe with space does not fail spuriously; there is nothing safe to do on
    // failure anyway, so the result is deliberately ignored.
    const ssize_t written = ::write(fd, &byte, 1);
    static_cast<void>(written);
  }

  errno = saved_errno;
}

// Drains the self-pipe, turning any interrupt into a stop request. Reports
// whether an interrupt was read. request_stop() is legal here because this runs
// on an ordinary thread, not in signal context.
bool drain_pipe() noexcept {
  bool interrupted = false;
  std::array<char, 64> bytes{};
  while (true) {
    const ssize_t count = ::read(g_read_fd, bytes.data(), bytes.size());
    if (count > 0) {
      interrupted = true;
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      break;  // Drained (EAGAIN), or an error the next read reports again.
    }
  }
  return interrupted;
}

// Close-on-exec on both ends, since the daemon may exec build tools and this is
// private plumbing. Non-blocking at both ends: a full pipe drops the byte
// rather than stalling a signal handler, and draining it never stalls the
// watch.
bool configure_pipe(const int read_fd, const int write_fd) {
  const auto configure = [](const int fd) {
    const int fd_flags = ::fcntl(fd, F_GETFD);
    const int status_flags = ::fcntl(fd, F_GETFL);
    return fd_flags != -1 && status_flags != -1 &&
           ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == 0 &&
           ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0;
  };
  return std::ranges::all_of(std::array{read_fd, write_fd}, configure);
}

// Throwing leaves g_pipe_created unset, so a later construction retries rather
// than inheriting a half-built pipe.
void create_wakeup_pipe() {
  std::array<int, 2> fds{};
  // pipe() plus fcntl rather than pipe2(), which would need _GNU_SOURCE.
  if (::pipe(fds.data()) != 0) {
    throw std::system_error(errno, std::system_category(), "pipe");
  }
  if (!configure_pipe(fds[0], fds[1])) {
    const int error = errno;
    ::close(fds[0]);
    ::close(fds[1]);
    throw std::system_error(error, std::system_category(), "fcntl");
  }
  g_read_fd = fds[0];
  g_wakeup_fd.store(fds[1], std::memory_order_release);
}

// Stops the watch on the self-pipe and drops anything left in it.
void stop_watching(stdexec::counting_scope& watching) {
  watching.request_stop();
  stdexec::sync_wait(watching.join());

  // The pipe outlives us, so a byte left by a handler firing during teardown
  // would reach the next watch as an interrupt nobody sent. The join means
  // nothing competes for the pipe here.
  static_cast<void>(drain_pipe());
}

// Unwinds a failed install. A failed sigaction() leaves its oldact
// unspecified, so `int_installed` says whether g_prev_int is worth restoring.
[[noreturn]] void fail_install(stdexec::counting_scope& watching,
                               const int error,
                               const bool int_installed) {
  if (int_installed) {
    ::sigaction(SIGINT, &g_prev_int, nullptr);
  }
  stop_watching(watching);
  throw std::system_error(error, std::system_category(),
                          "sigaction failed to install the console "
                          "interrupt handler");
}

// RAII guard for the single-instance `g_source`.
class SourceSlot {
 public:
  explicit SourceSlot(std::stop_source& source) {
    std::stop_source* expected = nullptr;
    if (!g_source.compare_exchange_strong(expected, &source,
                                          std::memory_order_acq_rel)) {
      throw std::logic_error("a ConsoleInterruptHandler already exists");
    }
  }

  ~SourceSlot() { g_source.store(nullptr, std::memory_order_release); }

  SourceSlot(const SourceSlot&) = delete;
  SourceSlot& operator=(const SourceSlot&) = delete;
};

}  // namespace

class ConsoleInterruptHandler::Impl {
  std::stop_source m_source;
  SourceSlot m_slot;

  // The watch on the self-pipe, stopped and joined on destruction.
  stdexec::counting_scope m_watching;

 public:
  explicit Impl(IoContext& io) : m_slot(m_source) {
    std::call_once(g_pipe_created, create_wakeup_pipe);

    // Started before the handlers are installed so a byte from an immediate
    // signal always has a reader - though the pipe would buffer it regardless.
    // Idempotent, so repeated interrupts are harmless.
    io.spawn_watch(g_read_fd, m_watching, []() noexcept {
      if (drain_pipe()) {
        if (std::stop_source* source = g_source.load(std::memory_order_acquire);
            source != nullptr) {
          source->request_stop();
        }
      }
    });

    struct sigaction action {};
    action.sa_handler = &handle_signal;
    sigemptyset(&action.sa_mask);
    // Let a system call the signal interrupts resume rather than fail.
    action.sa_flags = SA_RESTART;

    // Allowing testing for failure. Stands in for SIGTERM failing, which is
    // the case with a SIGINT install to undo.
    const std::optional<int> forced =
        std::exchange(g_forced_install_error, std::nullopt);

    if (::sigaction(SIGINT, &action, &g_prev_int) != 0) {
      fail_install(m_watching, forced.value_or(errno), false);
    }
    if (forced.has_value() ||
        ::sigaction(SIGTERM, &action, &g_prev_term) != 0) {
      fail_install(m_watching, forced.value_or(errno), true);
    }
  }

  ~Impl() {
    // First, so no later signal runs a handler for a source about to go away.
    ::sigaction(SIGINT, &g_prev_int, nullptr);
    ::sigaction(SIGTERM, &g_prev_term, nullptr);

    stop_watching(m_watching);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] std::stop_token token() const { return m_source.get_token(); }
};

ConsoleInterruptHandler::ConsoleInterruptHandler(IoContext& io)
    : m_impl(std::make_unique<Impl>(io)) {}

ConsoleInterruptHandler::~ConsoleInterruptHandler() = default;

std::stop_token ConsoleInterruptHandler::token() const {
  return m_impl->token();
}

void ConsoleInterruptHandlerTestUtil::fail_next_install(int error_code) {
  g_forced_install_error = error_code;
}

void ConsoleInterruptHandlerTestUtil::reset() {
  g_forced_install_error.reset();
}

}  // namespace makebelieve
