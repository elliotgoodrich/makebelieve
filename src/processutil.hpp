// SPDX-License-Identifier: MIT
#pragma once

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace makebelieve {

class IoContext;

/// @class ProcessUtil
/// Provides a namespace for running external commands.
struct ProcessUtil {
  /// What a command produced: the bytes it wrote to standard output, and the
  /// files it read while running - its discovered input dependencies.
  struct Output {
    std::string standard_output;

    /// Absolute paths, under @a working_directory, that the command (or
    /// anything it started) opened for reading, de-duplicated. Tracing is
    /// required, so this is complete: a run whose reads could not all be
    /// recorded fails rather than reporting fewer of them, and an empty list
    /// means the command really read nothing under @a working_directory.
    std::vector<std::filesystem::path> inputs;

    /// The status the command exited with - on Linux, 128 plus the signal
    /// that ended it, if one did, as a shell reports it. What a nonzero
    /// status means is for the caller to decide.
    int exit_status = 0;
  };

  /// The Output a command produced, or an `error_code` when it could not be
  /// run to completion.
  using Result = std::expected<Output, std::error_code>;

  /// Returns a sender that runs @a command through the system shell with the
  /// working directory set to @a working_directory, and completes with its
  /// standard output, the files it read (see Output::inputs) and its exit
  /// status once it has run to completion, whatever that status. It completes
  /// with `std::errc::operation_canceled` if @a stop was requested, in which
  /// case the command is terminated first, or with the platform error if the
  /// command could not be started or waited on - or could not be traced,
  /// whether because tracing could not be set up or because it failed while
  /// the command ran. A command that cannot be traced never runs, or is
  /// terminated.
  ///
  /// Whatever the command started and left running is asked to exit before
  /// the sender completes, but only on Windows does it wait until all of it
  /// has: there, everything the command started shares a job, whose processes
  /// can each be waited on. On Linux it kills what is still in the command's
  /// process group and does not wait - those processes are not its children,
  /// so it cannot reap them, and anything that left the group is not even
  /// killed. Waiting for every descendant there would take making this
  /// process a child subreaper, which has not been done.
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

/// @class ProcessUtilTestUtil
/// Test-only injection points for the ways `ProcessUtil::run` fails when a
/// command cannot be traced, which otherwise depend on the operating system
/// refusing - so that those paths, and that nothing is left behind after
/// them, can be exercised deterministically. Not intended for production use.
class ProcessUtilTestUtil {
 public:
  enum class Failure : int {
    /// Setting tracing up fails with the given error, before the command can
    /// run: on Linux, in the command's own process, before it does anything
    /// else; on Windows, before it is launched.
    tracing_setup,

    /// Linux: serving the command's FUSE mount fails with the given error at
    /// the first request.
    tracing_service,

    /// Windows: the trace log is gone by the time it is read back. The given
    /// error is not used.
    trace_log,

    /// Windows: the hook reports, in every process it is injected into, that
    /// it could not install. The given error is not used.
    trace_hook_install,

    /// Windows: the hook does nothing in any process it is injected into, as
    /// though it had never been. The given error is not used.
    trace_hook_absent,

    /// Windows: the hook works in the command itself but does nothing in
    /// anything the command starts. The given error is not used.
    trace_hook_absent_in_children,
  };

  /// Makes the next `ProcessUtil::run` to reach @a failure's step behave as
  /// though it failed there with @a error. One-shot: consumed by that run,
  /// and by nothing else. A failure a platform has no step for never fires.
  static void fail_next(Failure failure, std::error_code error = {});

  /// Clears a failure set by `fail_next()` that has not fired.
  static void reset();
};

}  // namespace makebelieve
