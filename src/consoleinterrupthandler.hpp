// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <stop_token>

namespace makebelieve {

/// @class ConsoleInterruptHandler
/// Installs a console interrupt handler that provides a
/// `std::stop_token` when the user interrupts the process
class ConsoleInterruptHandler {
  class Impl;
  std::unique_ptr<Impl> m_impl;

public:
  /// Creates a `ConsoleInterruptHandler` that will install a console interrupt handler
  /// or throw `std::system_error` one cannot be installed.
  /// @pre No other instance of this class is alive.
  ConsoleInterruptHandler();

  /// Uninstalls the console interrupt handler.
  ~ConsoleInterruptHandler();

  ConsoleInterruptHandler(const ConsoleInterruptHandler&) = delete;
  ConsoleInterruptHandler& operator=(const ConsoleInterruptHandler&) = delete;
  ConsoleInterruptHandler(ConsoleInterruptHandler&&) = delete;
  ConsoleInterruptHandler& operator=(ConsoleInterruptHandler&&) = delete;

  /// Provides a `std::stop_token` that will be signalled when the user
  /// interrupts the process. An interrupt that arrived before this method is called
  /// (but after the `ConsoleInterruptHandler` is constructed) is not missed.
  /// `std::stop_token`s returned from this function will not be triggered after
  /// the creating `ConsoleInterruptHandler` is destroyed.
  [[nodiscard]] std::stop_token token() const;
};

/// @class ConsoleInterruptHandlerTestUtil
/// Provides a test-only injection point for `ConsoleInterruptHandler`'s failure handling.
/// It exists so that the constructor's failure path, which otherwise depends on
/// the operating system refusing to install the handler, can be exercised
/// deterministically. It is not intended for production use.
class ConsoleInterruptHandlerTestUtil {
public:
  /// Makes the next `ConsoleInterruptHandler` construction behave as though the
  /// operating system call to install the handler failed with @a error_code.
  /// The injection is one-shot. It is consumed by the next construction attempt
  /// that reaches the install step, whether or not that attempt was going to
  /// fail on its own.
  static void fail_next_install(int error_code);

  /// Clears a pending injection previously set by `fail_next_install()`.
  static void reset();
};

}  // namespace makebelieve
