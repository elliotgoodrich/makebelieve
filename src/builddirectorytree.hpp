// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

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
/// one entry per declared `@/output <- command` rule. Each output is built
/// lazily: until it is first read it appears as a one-byte placeholder holding
/// a single null byte, and the first read of it hands the command to a
/// @link CommandRunner. When the runner reports the result, it replaces the
/// placeholder; later reads are served from that result.
///
/// This is deliberately the minimal slice of the build system: it does not
/// observe @a source for later changes, it does not rebuild an output once it
/// has been built successfully, and it does not track which inputs a command
/// consumed. @a source is only borrowed for the duration of the constructor.
class BuildDirectoryTree : public DirectoryTree {
  // Pimpl not necessary but kind of nice to keep things rebuilding quickly
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// The outcome of running a command: the bytes it produced, or an
  /// `error_code` when it could not be run to completion.
  using BuildResult = std::expected<std::string, std::error_code>;

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
      void(std::string command, std::stop_token stop, BuildComplete on_done)>;

  /// Builds the outputs declared in the `build.makebelieve` file of @a source,
  /// delegating each command to @a runner - any callable convertible to a
  /// @link CommandRunner. A @a source without a `build.makebelieve` file
  /// yields an empty tree.
  BuildDirectoryTree(const DirectoryTree& source, CommandRunner runner);

  ~BuildDirectoryTree() override;

  BuildDirectoryTree(const BuildDirectoryTree&) = delete;
  BuildDirectoryTree& operator=(const BuildDirectoryTree&) = delete;
  BuildDirectoryTree(BuildDirectoryTree&&) = delete;
  BuildDirectoryTree& operator=(BuildDirectoryTree&&) = delete;

  /// Returns a `CommandRunner` that substitutes `%out` with a scratch file,
  /// runs the command through the system shell with the working directory set
  /// to @a working_directory (so relative inputs resolve against it), and
  /// reports the bytes the command wrote to that file. It runs the command
  /// synchronously, reporting the result before returning.
  [[nodiscard]] static CommandRunner shell_runner(
      std::filesystem::path working_directory);

  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override;

  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
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
