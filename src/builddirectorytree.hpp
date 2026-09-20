// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"
#include "manifest.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace makebelieve {

/// @class BuildDirectoryTree
/// A `DirectoryTree` whose contents are the outputs declared in a
/// `build.makebelieve` manifest that lives in another `DirectoryTree`.
///
/// On construction it reads the manifest from a @a source tree and presents
/// one entry per declared `@/output = <action> <command>` rule. `open` builds
/// an output that is unbuilt or dirty, blocking until its @link CommandRunner
/// reports back. `status` and `read` never build: an unbuilt output reports
/// size 1, otherwise the size of its last build.
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

  /// One rule's right-hand side: the command to run, and the action that says
  /// where its output comes from - the file `%out` names for
  /// `Manifest::Action::Run`, its standard output for
  /// `Manifest::Action::Capture`.
  struct Command {
    Manifest::Action action = Manifest::Action::Run;
    std::string text;

    [[nodiscard]] friend bool operator==(const Command&,
                                         const Command&) = default;
  };

  /// Reports the outcome of a single command back to the tree. Move-only so it
  /// can carry move-only state, and single-shot - call it exactly once.
  using BuildComplete = std::move_only_function<void(BuildResult)>;

  /// Runs a command and reports the bytes it produced through the completion
  /// handler. The runner may call the handler synchronously, before returning,
  /// or later from another thread; either way it must call it exactly once.
  /// The `stop_token` is requested when the result is no longer wanted (for
  /// instance when the tree is being destroyed), and a runner that defers work
  /// should honour it and still complete - reporting
  /// `std::errc::operation_canceled` is fine. A runner that does not care about
  /// cancellation can simply leave the `stop_token` parameter unnamed.
  /// @pre A deferred completion does not outlive this `BuildDirectoryTree`.
  using CommandRunner = std::function<
      void(Command command, std::stop_token stop, BuildComplete on_done)>;

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

  ~BuildDirectoryTree() override;

  BuildDirectoryTree(const BuildDirectoryTree&) = delete;
  BuildDirectoryTree& operator=(const BuildDirectoryTree&) = delete;
  BuildDirectoryTree(BuildDirectoryTree&&) = delete;
  BuildDirectoryTree& operator=(BuildDirectoryTree&&) = delete;

  /// Returns a `CommandRunner` that runs the command through the system shell
  /// with the working directory set to @a working_directory (so relative
  /// inputs resolve against it). A `Run` command has `%out` substituted with a
  /// scratch file and produces the bytes it wrote there; a `Capture` command
  /// produces the bytes it wrote to standard output. It runs the command
  /// synchronously, reporting the result before returning. Traced inputs are
  /// reported relative to @a working_directory, so for dependency tracking to
  /// line up it should be the filesystem root that @a source mirrors.
  [[nodiscard]] static CommandRunner shell_runner(
      std::filesystem::path working_directory);

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
