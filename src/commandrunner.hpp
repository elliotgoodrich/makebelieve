// SPDX-License-Identifier: MIT
#pragma once

#include "launchregistrar.hpp"
#include "manifest.hpp"

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <new>
#include <string>
#include <system_error>
#include <vector>

// What a `BuildDirectoryTree` hands a runner and what the runner hands back:
// the contract between deciding what to build and carrying a build out.

namespace makebelieve {

/// What running a command produced: the bytes of the output it built, and the
/// input files it read (its dependencies), relative to the source tree's
/// root.
struct BuildOutput {
  std::string bytes;
  std::vector<std::filesystem::path> inputs;
};

/// The outcome of running a command: a @link BuildOutput, or an `error_code`
/// when it could not be run to completion.
using BuildResult = std::expected<BuildOutput, std::error_code>;

/// One rule as the runner receives it: the output it builds, the action that
/// says where that output comes from - the file `%out` names for
/// `Manifest::Action::Run`, the command's standard output for
/// `Manifest::Action::Capture`, a file read as-is for
/// `Manifest::Action::Copy` - and the text that action applies to.
struct Command {
  /// The output this rule builds, relative to the build tree's root. A runner
  /// that hands the command a scratch file gives it this one's file name, so
  /// a tool that picks its format from the extension (pandoc and friends)
  /// sees the name the finished output will have.
  std::filesystem::path output;

  Manifest::Action action = Manifest::Action::Run;

  /// The command to run for `Run` and `Capture`; for `Copy`, the path of the
  /// file to read, relative to the source tree's root; empty for `Tracing`.
  std::string text;

  [[nodiscard]] friend bool operator==(const Command&,
                                       const Command&) = default;
};

/// A build under way, as a sender: it completes with the @link BuildResult,
/// or with an `exception_ptr` or stopped, both of which a `BuildDirectoryTree`
/// records as a failed build. The tree cancels it through the stop token in the
/// receiver's environment when the result is no longer wanted (for instance
/// when the tree is being destroyed); a build that honours it should still
/// complete promptly, and completing stopped is fine. It must not complete
/// from within its stop callback, on the thread requesting the stop: the
/// stop sources stdexec interposes live inside the build and would be freed
/// while that request is still walking them. Completing from another thread
/// is fine.
///
/// The receiver's environment also names the @link LaunchRegistrar every
/// process the build launches must be registered with before it runs
/// (`get_launch_registrar`). A build that launches nothing may ignore it; one
/// that does must pass it on, so the processes a build runs can be told apart
/// from anyone else's.
using BuildSender = exec::any_sender<exec::any_receiver<
    stdexec::completion_signatures<stdexec::set_value_t(BuildResult),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>,
    exec::queries<stdexec::inplace_stop_token(
                      stdexec::get_stop_token_t) noexcept,
                  LaunchRegistrar(get_launch_registrar_t) noexcept>>>;

/// The error a build that completed with the exception @a error is recorded
/// as.
[[nodiscard]] inline std::error_code to_error_code(
    const std::exception_ptr& error) noexcept {
  try {
    std::rethrow_exception(error);
  } catch (const std::system_error& failure) {
    return failure.code();
  } catch (const std::bad_alloc&) {
    return std::make_error_code(std::errc::not_enough_memory);
  } catch (...) {
    return std::make_error_code(std::errc::io_error);
  }
}

/// Carries out one rule, returning the build as a sender that the caller -
/// a `BuildDirectoryTree` - starts. A `Manifest::Action::Tracing` rule never
/// reaches it. The sender may complete inline, from within `start`, or later on
/// any thread. Any sender whose completions fit @link BuildSender converts to
/// it, so a runner with its answer to hand can return `stdexec::just(result)`.
using CommandRunner = std::function<BuildSender(Command command)>;

}  // namespace makebelieve
