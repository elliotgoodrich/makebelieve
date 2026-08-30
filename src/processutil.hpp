// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace makebelieve {

/// @class ProcessUtil
/// Provides a namespace for running external commands.
struct ProcessUtil {
  /// What a command produced: the bytes it wrote to standard output, and the
  /// files it read while running - its discovered input dependencies.
  struct Output {
    std::string standard_output;

    /// Absolute paths, under @a working_directory, that the command (or a
    /// descendant it spawned) opened for reading, de-duplicated. Discovering
    /// these needs OS-level tracing, so it is best-effort: on a platform where
    /// tracing is not yet wired up - or when the tracer could not attach - this
    /// is empty even though the command really did read files. Never treat an
    /// empty list as proof a command read nothing.
    std::vector<std::filesystem::path> inputs;
  };

  /// The Output a command produced, or an `error_code` when it could not be
  /// run to completion.
  using Result = std::expected<Output, std::error_code>;

  /// Reports the outcome of a single command.
  using Complete = std::move_only_function<void(Result)>;

  /// Runs @a command through the system shell with the working directory set
  /// to @a working_directory, captures its standard output and the files it
  /// read (see Output::inputs), and reports both through @a on_done. If @a stop
  /// is requested a best-effort attempt is made to terminate the command.
  ///
  /// @a on_done receives the Output once the command has run to completion
  /// (whatever its exit status), `std::errc::operation_canceled` if it was
  /// terminated because @a stop was requested, or the platform error if it
  /// could not be launched or waited on. It is called exactly once,
  /// synchronously, before this returns.
  ///
  /// Note this currently blocks and should be improved later on.
  static void run(const std::filesystem::path& working_directory,
                  const std::string& command,
                  const std::stop_token& stop,
                  Complete on_done);
};

}  // namespace makebelieve
