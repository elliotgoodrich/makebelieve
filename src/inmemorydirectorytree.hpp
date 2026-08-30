// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace makebelieve {

/// @class InMemoryDirectoryTree
/// A `DirectoryTree` backed entirely by memory, with manipulators to build
/// and mutate its contents directly.
///
/// Thread-safe: readers (`status`/`ls`/`read`) run concurrently; mutators
/// serialise against them and each other. Change notifications are delivered
/// with no lock held, so a callback may read back through the tree - but a
/// concurrent mutation may by then have moved the tree past the state the diff
/// describes.
class InMemoryDirectoryTree : public DirectoryTree {
  // Pimpl idiom not really needed, but this class is used for testing
  // so the extra indirection isn't critical and pimpl allows us to avoid
  // some include directives in the header.
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// Creates a tree containing just an empty root directory.
  InMemoryDirectoryTree();

  ~InMemoryDirectoryTree() override;

  InMemoryDirectoryTree(const InMemoryDirectoryTree&) = delete;
  InMemoryDirectoryTree& operator=(const InMemoryDirectoryTree&) = delete;
  InMemoryDirectoryTree(InMemoryDirectoryTree&&) = delete;
  InMemoryDirectoryTree& operator=(InMemoryDirectoryTree&&) = delete;

  /// Creates or overwrites the file at @a path with @a content.
  /// @pre The parent directory of @a path exists.
  /// @pre @a path does not already name a directory.
  void write_file(const std::filesystem::path& path,
                  std::string_view content,
                  std::chrono::file_clock::time_point mtime =
                      std::chrono::file_clock::now());

  /// Creates an empty directory at @a path.
  /// @pre The parent directory of @a path exists.
  /// @pre No entry already exists at @a path.
  void make_directory(const std::filesystem::path& path,
                      std::chrono::file_clock::time_point mtime =
                          std::chrono::file_clock::now());

  /// Removes the file or directory at @a path. A directory is removed
  /// together with everything beneath it.
  /// @pre An entry exists at @a path.
  /// @pre @a path is not the root.
  void remove(const std::filesystem::path& path);

  /// Updates the mtime of the file or directory at @a path (the root,
  /// included) without touching its content or children.
  /// @pre @a path is the root, or an entry exists at @a path.
  void set_mtime(const std::filesystem::path& path,
                 std::chrono::file_clock::time_point mtime);

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
