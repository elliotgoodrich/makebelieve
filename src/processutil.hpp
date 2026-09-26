// SPDX-License-Identifier: MIT
#pragma once

#include "iocontext.hpp"

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
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

  /// This process's own id.
  [[nodiscard]] static std::uint32_t self();

  /// The name of the executable the process @a pid is running, or `unknown`
  /// where it has exited or this process may not query it.
  [[nodiscard]] static std::string name_of(std::uint32_t pid);

  /// Returns a sender that runs @a command through the system shell with the
  /// working directory set to @a working_directory, and completes with its
  /// standard output and the files it read (see Output::inputs) once it has
  /// run to completion, whatever its exit status. It completes with
  /// `std::errc::operation_canceled` if @a stop was requested, in which case
  /// the command and everything it started are terminated first, or with the
  /// platform error if the command could not be started or waited on.
  ///
  /// The sender does its brief synchronous work - starting the command,
  /// reading back what it traced - on whatever thread it is started on, and
  /// waits on the command through @a io without holding a thread, resuming
  /// where it started. It ignores stop requests from whatever awaits it:
  /// cancelling is @a stop's job, so that it always completes only once the
  /// command has gone.
  /// @pre @a io outlives the sender's run, as does the source of @a stop.
  [[nodiscard]] static auto run(IoContext& io,
                                std::filesystem::path working_directory,
                                std::string command,
                                stdexec::inplace_stop_token stop = {}) {
    return stdexec::write_env(
        run_task(io, std::move(working_directory), std::move(command), stop),
        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}});
  }

 private:
  static exec::task<Result> run_task(IoContext& io,
                                     std::filesystem::path working_directory,
                                     std::string command,
                                     stdexec::inplace_stop_token stop);
};

}  // namespace makebelieve
