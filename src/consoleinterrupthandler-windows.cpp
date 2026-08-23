// SPDX-License-Identifier: MIT
#include "consoleinterrupthandler.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <windows.h>

namespace makebelieve {

namespace {

std::atomic<std::stop_source*> g_source{nullptr};

// Test global set by `ConsoleInterruptHandlerTestUtil`. When
// it holds a value, the next construction throws as though the handler could
// not be installed.
std::optional<int> g_forced_install_error;

// Runs on a thread the OS injects into the process rather than on any thread
// of ours. That matters: it is an ordinary thread, so request_stop() - which
// takes a lock and runs callbacks, and is *not* async-signal-safe - is legal
// to call here. The POSIX backend will not have that luxury and will need to
// hand off from the signal handler before requesting a stop.
BOOL WINAPI console_handler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT: [[fallthrough]];
    case CTRL_BREAK_EVENT: [[fallthrough]];
    case CTRL_CLOSE_EVENT: [[fallthrough]];
    case CTRL_LOGOFF_EVENT: [[fallthrough]];
    case CTRL_SHUTDOWN_EVENT: {
      if (std::stop_source* source = g_source.load(std::memory_order_acquire)) {
        source->request_stop();
        // Returning TRUE for CTRL_C_EVENT and CTRL_BREAK_EVENT suppresses the
        // default "terminate immediately", which is what buys the main thread the
        // time to react to the `std::stop_token` and shut down cleanly.
        //
        // For CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT and CTRL_SHUTDOWN_EVENT, Windows
        // terminates the process once this returns, allowing only a short grace
        // period. Teardown races that deadline, so a console window closed rather
        // than interrupted may not have time to react to the `std::stop_token` before the process is killed.
        return TRUE;
      }
    }
  }

  // Otherwise, let the next handler in the chain decide.
  return FALSE;
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

public:
  Impl(): m_source(), m_slot(m_source) {
    // Test seam: a pending injection behaves as though the install below
    // failed, without an actual OS failure. Consumed whether or not it fires.
    // See ConsoleInterruptHandlerTestUtil::fail_next_install().
    if (const std::optional<int> forced =
            std::exchange(g_forced_install_error, std::nullopt)) {
      throw std::system_error(*forced, std::system_category(),
                              "SetConsoleCtrlHandler failed to install the console interrupt handler");
    }

    if (!::SetConsoleCtrlHandler(console_handler, TRUE)) {
      const DWORD error = ::GetLastError();
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "SetConsoleCtrlHandler failed to install the console interrupt handler");
    }
  }

  ~Impl() {
    ::SetConsoleCtrlHandler(console_handler, FALSE);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  std::stop_token token() const { return m_source.get_token(); }
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
