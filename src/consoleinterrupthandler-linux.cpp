// SPDX-License-Identifier: MIT
#include "consoleinterrupthandler.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::atomic<std::stop_source*> g_source{nullptr};

// Test global set by `ConsoleInterruptHandlerTestUtil`. When
// it holds a value, the next construction throws as though the handler could
// not be installed. Test-only, so no need to be atomic.
std::optional<int> g_forced_install_error;

// The self-pipe between the signal handler and the dispatcher thread. The
// handler touches only the write end, and only via write(2), which is
// async-signal-safe; the dispatcher owns the read end.
//
// Created once and never closed: sigaction() does not wait for a handler that
// is already running, so a closed write end could be reopened as something
// else and catch that handler's byte. Two descriptors is the price of that.
std::atomic<int> g_wakeup_fd{-1};
int g_read_fd = -1;
std::once_flag g_pipe_created;

// What makes g_wakeup_fd legal to touch from a signal handler.
static_assert(std::atomic<int>::is_always_lock_free);

// Sent through the pipe to distinguish an interrupt from teardown.
constexpr char k_interrupt = 'I';
constexpr char k_quit = 'Q';

// Previous dispositions, restored on teardown so a signal arriving afterwards
// takes the default action instead of running a handler for a stop source that
// no longer exists.
struct sigaction g_prev_int {};
struct sigaction g_prev_term {};

// Runs in signal context, so it writes to the self-pipe and lets the dispatcher
// thread do the rest. request_stop() takes a lock and runs callbacks and so is *not* legal here,
// which is the whole reason this backend needs a thread.
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

// Blocks on the self-pipe, turning an interrupt byte into a stop request and
// the quit byte into a clean exit. request_stop() is legal here because this
// is an ordinary thread, not signal context.
void dispatch_loop() {
  while (true) {
    char byte = 0;
    const ssize_t count = ::read(g_read_fd, &byte, 1);
    if (count <= 0) {
      if (count < 0 && errno == EINTR) {
        continue;
      }
      return;  // Pipe closed, or an error we cannot recover from.
    }
    if (byte == k_quit) {
      return;
    }
    if (std::stop_source* source = g_source.load(std::memory_order_acquire);
        source != nullptr) {
      // Idempotent, so repeated interrupts are harmless; we keep looping so a
      // later k_quit still reaches us.
      source->request_stop();
    }
  }
}

// Close-on-exec on both ends, since the daemon may exec build tools and this is
// private plumbing. Non-blocking write end so a full pipe drops the byte rather
// than stalling a signal handler.
bool configure_pipe(const int read_fd, const int write_fd) {
  for (const int fd : {read_fd, write_fd}) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags == -1 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
      return false;
    }
  }
  const int flags = ::fcntl(write_fd, F_GETFL);
  return flags != -1 && ::fcntl(write_fd, F_SETFL, flags | O_NONBLOCK) == 0;
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

// Wakes the dispatcher with the quit sentinel, joins it, and drops anything
// left in the pipe. Only EINTR and EAGAIN can fail the write: the descriptor is
// never closed, and the dispatcher is still draining, so a full pipe clears.
void stop_dispatcher(std::thread& dispatcher) {
  const char quit = k_quit;
  while (::write(g_wakeup_fd.load(std::memory_order_relaxed), &quit, 1) < 0 &&
         (errno == EINTR || errno == EAGAIN)) {
  }

  if (dispatcher.joinable()) {
    dispatcher.join();
  }

  // The pipe outlives us, so a byte left by a handler firing during teardown
  // would reach the next dispatcher as an interrupt nobody sent. The join means
  // nothing competes for the pipe here.
  struct pollfd waiting{.fd = g_read_fd, .events = POLLIN, .revents = 0};
  char discarded = 0;
  while (::poll(&waiting, 1, 0) == 1 &&
         ::read(g_read_fd, &discarded, 1) == 1) {
  }
}

// Unwinds a failed install. A failed sigaction() leaves its oldact
// unspecified, so `int_installed` says whether g_prev_int is worth restoring.
[[noreturn]] void fail_install(std::thread& dispatcher, const int error,
                               const bool int_installed) {
  if (int_installed) {
    ::sigaction(SIGINT, &g_prev_int, nullptr);
  }
  stop_dispatcher(dispatcher);
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

}  // close anonymous namespace

class ConsoleInterruptHandler::Impl {
  std::stop_source m_source;
  SourceSlot m_slot;
  std::thread m_dispatcher;

public:
  Impl(): m_slot(m_source) {
    std::call_once(g_pipe_created, create_wakeup_pipe);

    // Started before the handlers are installed so a byte from an immediate
    // signal always has a reader - though the pipe would buffer it regardless.
    m_dispatcher = std::thread(dispatch_loop);

    struct sigaction action {};
    action.sa_handler = &handle_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;  // Let the dispatcher's read() resume, not fail.

    // Allowing testing for failure. Stands in for SIGTERM failing, which is
    // the case with a SIGINT install to undo.
    const std::optional<int> forced =
        std::exchange(g_forced_install_error, std::nullopt);

    if (::sigaction(SIGINT, &action, &g_prev_int) != 0) {
      fail_install(m_dispatcher, forced.value_or(errno), false);
    }
    if (forced.has_value() ||
        ::sigaction(SIGTERM, &action, &g_prev_term) != 0) {
      fail_install(m_dispatcher, forced.value_or(errno), true);
    }
  }

  ~Impl() {
    // First, so no later signal runs a handler for a source about to go away.
    ::sigaction(SIGINT, &g_prev_int, nullptr);
    ::sigaction(SIGTERM, &g_prev_term, nullptr);

    stop_dispatcher(m_dispatcher);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] std::stop_token token() const { return m_source.get_token(); }
};

ConsoleInterruptHandler::ConsoleInterruptHandler()
    : m_impl(std::make_unique<Impl>()) {}

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
