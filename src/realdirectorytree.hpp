// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace makebelieve {

/// @class RealDirectoryTree
/// A concrete implementation of @link DirectoryTree that represents a real
/// directory on disk.
class RealDirectoryTree : public DirectoryTree {
  class Impl;
  std::unique_ptr<Impl> m_impl;

public:
  /// Creates a `RealDirectoryTree` at @a root.  If @a root does not exist
  /// then construction still succeeds and every query fails with
  /// `std::errc::no_such_file_or_directory`.
  explicit RealDirectoryTree(const std::filesystem::path& root);

  ~RealDirectoryTree() override;

  RealDirectoryTree(const RealDirectoryTree&) = delete;
  RealDirectoryTree& operator=(const RealDirectoryTree&) = delete;
  RealDirectoryTree(RealDirectoryTree&&) = delete;
  RealDirectoryTree& operator=(RealDirectoryTree&&) = delete;

  /// Returns entry at @a path, or an error_code when it is missing, unreachable, or
  /// escapes the root.
  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override;

  /// Returns the children of @a path, or an error_code when it cannot be listed. An
  /// existing but empty directory yields an empty vector.
  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path) const override;

  /// Returns up to @a size bytes of @a path starting at @a offset, or an error_code on
  /// failure. A result shorter than @a size means end of file. A @a size of 0 succeeds
  /// with an empty result without opening @a path.
  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size) const override;

  /// Subscribes to changes in this object by calling @a callback and returns an
  /// RAII guard that will unsubscribe on destruction.
  /// @note Paths in the diff are relative to the tree root.
  /// @pre The returned object does not outlive this `DirectoryTree`.
  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback)
      const override;
};

}  // namespace makebelieve
