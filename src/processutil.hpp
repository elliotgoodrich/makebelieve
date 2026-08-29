// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <system_error>

namespace makebelieve {

/// @class ProcessUtil
/// Provides a namespace for running external commands.
struct ProcessUtil {
  /// The standard output a command produced, or an `error_code` when it could
  /// not be run to completion.
  using Result = std::expected<std::string, std::error_code>;

  /// Reports the outcome of a single command.
  using Complete = std::move_only_function<void(Result)>;

  /// Runs @a command through the system shell with the working directory set
  /// to @a working_directory, captures its standard output, and reports it
  /// through @a on_done. If @a stop is requested a best-effort attempt
  /// is made to terminate the command.
  ///
  /// @a on_done receives the captured standard output once the command has run
  /// to completion (whatever its exit status), `std::errc::operation_canceled`
  /// if it was terminated because @a stop was requested, or the platform error
  /// if it could not be launched or waited on. It is called exactly once,
  /// synchronously, before this returns.
  ///
  /// Note this currently blocks and should be improved later on.
  static void run(const std::filesystem::path& working_directory,
                  const std::string& command,
                  const std::stop_token& stop,
                  Complete on_done);
};

}  // namespace makebelieve
