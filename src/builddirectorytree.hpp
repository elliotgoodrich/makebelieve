// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"
#include "manifest.hpp"

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace makebelieve {

/// @class BuildDirectoryTree
/// A `DirectoryTree` whose contents are the outputs declared in a
/// `build.makebelieve` manifest that lives in another `DirectoryTree`.
///
/// On construction it reads the manifest from a @a source tree and presents
/// one entry per declared `@/output = <action> <argument>` rule. `open` builds
/// an output that is unbuilt or dirty, blocking until the build its
/// @link CommandRunner returned completes. `status` and `read` never build: an
/// unbuilt output reports size 1, otherwise the size of its last build.
///
/// It then observes @a source for the rest of its life, recording the inputs
/// each build reads, so a change to one of those inputs rebuilds the outputs
/// that depend on it: eagerly if opened before, otherwise on the next open.
/// @a source must outlive this tree.
///
/// A change to `build.makebelieve` itself reloads the rules: a new rule's
/// output appears unbuilt, a removed rule's output disappears (with any
/// directories left empty), and an output whose command changed is rebuilt as
/// if an input had changed. Outputs whose rule is unchanged keep their content.
/// A manifest that cannot be read, or has any line @link Manifest::parse
/// rejects, is not applied: the current rules stay in force until it is fixed.
///
/// A `tracing` rule's output is the trace recorded to @link g_tracer (or an
/// empty trace when there is none), served by the tree itself rather than the
/// runner. Every open takes a fresh snapshot, and since that rewrite is caused
/// by the reader it is not announced to subscribers.
///
/// Subscribers are notified while an internal lock is held, so they must not
/// call `open` from within their callback.
class BuildDirectoryTree : public DirectoryTree {
  // Pimpl not necessary but kind of nice to keep things rebuilding quickly
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// What running a command produced: the bytes of the output it built, and the
  /// input files it read (its dependencies), relative to the source tree's
  /// root. Best-effort, and empty where tracing is unavailable.
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
    /// The output this rule builds, relative to this tree's root. A runner
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
  /// or with an `exception_ptr` or stopped, both of which the tree records as
  /// a failed build. The tree cancels it through the stop token in the
  /// receiver's environment when the result is no longer wanted (for instance
  /// when the tree is being destroyed); a build that honours it should still
  /// complete promptly, and completing stopped is fine. It must not complete
  /// from within its stop callback, on the thread requesting the stop: the
  /// stop sources stdexec interposes live inside the build and would be freed
  /// while that request is still walking them. Completing from another thread
  /// is fine.
  using BuildSender = exec::any_sender<exec::any_receiver<
      stdexec::completion_signatures<stdexec::set_value_t(BuildResult),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>,
      exec::queries<stdexec::inplace_stop_token(
          stdexec::get_stop_token_t) noexcept>>>;

  /// Carries out one rule, returning the build as a sender that the tree
  /// starts. A `Manifest::Action::Tracing` rule never reaches it. The sender
  /// may complete inline, from within `start`, or later on any thread. Any
  /// sender whose completions fit @link BuildSender converts to it, so a
  /// runner with its answer to hand can return `stdexec::just(result)`.
  using CommandRunner = std::function<BuildSender(Command command)>;

  /// Told why a changed manifest was rejected, one problem per line (such as
  /// `build.makebelieve:3: expected ...`). Called on the source's watcher
  /// thread.
  using ManifestErrorHandler = std::function<void(const std::string& problems)>;

  /// Builds the outputs declared in the `build.makebelieve` file of @a source,
  /// delegating each command to @a runner - any callable convertible to a
  /// @link CommandRunner. A @a source without a `build.makebelieve` file
  /// yields an empty tree. Throws `std::runtime_error` describing the problems
  /// if the manifest cannot be read or has lines it cannot accept; later
  /// problems of that kind go to @a on_manifest_error, if given.
  /// @pre @a source outlives this tree; it is observed for its whole life.
  BuildDirectoryTree(const DirectoryTree& source,
                     CommandRunner runner,
                     ManifestErrorHandler on_manifest_error = {});

  /// Cancels every build still under way and waits for each to complete, so
  /// no build outlives the tree.
  ~BuildDirectoryTree() override;

  BuildDirectoryTree(const BuildDirectoryTree&) = delete;
  BuildDirectoryTree& operator=(const BuildDirectoryTree&) = delete;
  BuildDirectoryTree(BuildDirectoryTree&&) = delete;
  BuildDirectoryTree& operator=(BuildDirectoryTree&&) = delete;

  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override;

  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path) const override;

  /// Builds @a path if unbuilt or dirty (or waits for a build under way) and
  /// returns its info once up to date, or the error of a failed build.
  [[nodiscard]] std::expected<FileInfo, std::error_code> open(
      const std::filesystem::path& path) const override;

  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size) const override;

  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback)
      const override;
};

}  // namespace makebelieve
