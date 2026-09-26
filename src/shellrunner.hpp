// SPDX-License-Identifier: MIT
#pragma once

#include "commandrunner.hpp"
#include "iocontext.hpp"

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <filesystem>
#include <utility>

namespace makebelieve {

namespace detail {

// Carries out @a command as `ShellRunner` describes, waiting on a command it
// runs through @a io. Its synchronous steps run wherever the task is started,
// and it resumes there after each wait. @a stop cancels a command still
// running.
exec::task<BuildResult> run_shell_command(
    IoContext& io,
    std::filesystem::path working_directory,
    Command command,
    stdexec::inplace_stop_token stop);

}  // namespace detail

/// @class ShellRunner
/// A `CommandRunner` that carries out each rule it is
/// given, with no limit of its own on how many run at once. Its
/// brief synchronous steps - preparing and removing a scratch directory,
/// starting a command, reading back what it wrote - run on @a Scheduler, and
/// it waits on a command through an `IoContext`, holding no thread while it
/// runs.
///
/// Each rule runs through the system shell with the working directory set to
/// the one given at construction, so relative inputs resolve against it. A
/// `Run` command has `%out` substituted with a scratch file - with the same
/// file name as the output it builds, so a tool that chooses its format from
/// the extension needs no extra flag - and produces the bytes it wrote there;
/// a `Capture` command produces the bytes it wrote to standard output. A
/// `Copy` rule runs no command at all: it produces the bytes of the file it
/// names, read from under the working directory, and reports that file as its
/// one input.
/// Traced inputs are reported relative to the working directory, so for
/// dependency tracking to line up it should be the filesystem root that the
/// tree's source mirrors.
///
/// @tparam Scheduler Where each build's synchronous steps run, and where it
/// resumes, and completes, after waiting on its command. Satisfying
/// `stdexec::scheduler` is not enough: those steps block - on the filesystem,
/// on starting a process, on reading back an output that may be large - so
/// @a Scheduler must run work on threads meant for that, such as a worker
/// pool's. An inline scheduler, or any whose work runs on the thread that
/// started it, would leave them on the caller's thread, which may be the
/// `IoContext`'s or a filesystem dispatcher's.
template <stdexec::scheduler Scheduler>
class ShellRunner {
  std::filesystem::path m_working_directory;
  IoContext* m_io;
  Scheduler m_scheduler;

 public:
  /// Creates a runner working in @a working_directory, running its
  /// synchronous steps on @a scheduler and waiting through @a io.
  /// @pre @a io, and whatever runs @a scheduler's work, outlive every build it
  /// starts.
  ShellRunner(std::filesystem::path working_directory,
              IoContext& io,
              Scheduler scheduler)
      : m_working_directory(std::move(working_directory)),
        m_io(&io),
        m_scheduler(std::move(scheduler)) {}

  /// The build of @a command, as this class describes.
  [[nodiscard]] BuildSender operator()(Command command) const {
    // Started on the scheduler, where the task then resumes after each wait.
    // Spelled as schedule-then-let_value because stdexec cannot type-erase a
    // starts_on onto some schedulers, a static_thread_pool's among them.
    return stdexec::schedule(m_scheduler) |
           stdexec::let_value([io = m_io,
                               working_directory = m_working_directory,
                               command = std::move(command)]() mutable {
             return stdexec::read_env(stdexec::get_stop_token) |
                    stdexec::let_value([&](stdexec::inplace_stop_token stop) {
                      return detail::run_shell_command(
                          *io, working_directory, std::move(command), stop);
                    });
           });
  }
};

}  // namespace makebelieve
