// SPDX-License-Identifier: MIT
#pragma once

#include "commandrunner.hpp"
#include "directorytree.hpp"
#include "manifest.hpp"

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

class BuildCoordinator;

/// @class BuildDirectoryTree
/// A `DirectoryTree` whose contents are the outputs declared in a
/// `build.makebelieve` manifest that lives in another `DirectoryTree`.
///
/// On construction it reads the manifest from a @a source tree and presents
/// one entry per declared `@/output = <action> <argument>` rule. `open` builds
/// an output that is unbuilt or dirty, waiting until the build its
/// @link CommandRunner returned completes. `open` suspends instead of
/// blocking its caller; filesystem backends use that interface. `status` and
/// `read` never build: an unbuilt output reports size 1, otherwise the size of
/// its last build.
///
/// Each build is one generation of its output, run through a
/// @link BuildCoordinator, which is what an `open` waits through. An open
/// waits for the build under way if the output has not changed since that
/// build started, and gets that build's result even if the output changes
/// before it finishes; if it has changed, the open waits for the build after
/// it instead. (This is deliberate, and new: an open used to wait on through
/// build after build for as long as the output kept changing.) Content read
/// afterwards may already be a later build's: reads are not snapshotted.
/// Opens made by a build's own command (as the coordinator tells them apart)
/// give its permit back while they wait, and one that would wait on a cycle of
/// builds fails with `std::errc::resource_deadlock_would_occur`.
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

  /// As above, but running and waiting on builds through @a coordinator
  /// rather than one of its own, which places no limit on how many run at
  /// once and attributes no request to any build.
  /// @pre @a coordinator outlives this tree.
  BuildDirectoryTree(const DirectoryTree& source,
                     CommandRunner runner,
                     BuildCoordinator& coordinator,
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

  using DirectoryTree::open;

  /// Builds @a path if unbuilt or dirty (or waits for a build under way) and
  /// returns its info once up to date, or the error of the failed build it
  /// waited on - or fails as the coordinator's `wait` does, for a request
  /// @a context says it cannot let wait.
  [[nodiscard]] OpenSender open(const std::filesystem::path& path,
                                const OpenContext& context = {}) const override;
  /// Returns a sender reading up to @a size bytes at @a offset from @a path's
  /// last published build. Does not build; await @link open first.
  [[nodiscard]] ReadSender read(const std::filesystem::path& path,
                                Offset offset,
                                std::size_t size) const override;

  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback)
      const override;
};

}  // namespace makebelieve
