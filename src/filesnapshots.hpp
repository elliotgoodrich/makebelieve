// SPDX-License-Identifier: MIT
#pragma once
#include "directorytree.hpp"

#include <exec/task.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>

namespace makebelieve {

/// \class FileSnapshot
/// One version of a file's content, as it was when taken. It never changes, so
/// everything holding it reads the same bytes however the file changes after.
struct FileSnapshot {
  /// The file this is a version of.
  std::filesystem::path path;
  /// As DirectoryTree::read_whole() returned it, so possibly shared with the
  /// tree itself.
  std::shared_ptr<const std::string> bytes;
  std::chrono::file_clock::time_point mtime;

  /// The @link FileSnapshots::generation of the file this was taken at.
  std::uint64_t generation = 0;

  [[nodiscard]] const std::string& content() const { return *bytes; }

  [[nodiscard]] FileInfo info() const {
    return {.size = bytes->size(), .mtime = mtime};
  }
};

/// \class FileSnapshots
/// Takes snapshots of the files in a DirectoryTree, sharing one between
/// everyone who takes the same version of a file and keeping each for as long
/// as anyone holds it - the way a file a build replaces on disk lives on for
/// whoever still has it open.
///
/// A snapshot is taken with DirectoryTree::read_whole(), and reused only while
/// the tree itself still reports the same file, whenever the tree's change
/// notifications arrive. Those only number the versions.
class FileSnapshots {
 public:
  /// @pre @a tree outlives this object.
  explicit FileSnapshots(const DirectoryTree& tree);

  /// Moves on the generation of every file @a diff names, or of every file if
  /// it says everything is dirty. Call it from the tree's change subscription.
  void changed(const DirectoryTreeDiff& diff);

  /// A number that grows each time @a path is reported changed, starting at 0
  /// for a file never reported changed.
  [[nodiscard]] std::uint64_t generation(
      const std::filesystem::path& path) const;

  /// The newest generation of any file: no file's is greater.
  [[nodiscard]] std::uint64_t latest() const;

  /// The content of the file at @a path as it is now, shared with whoever
  /// already holds that version. Await DirectoryTree::open() first for final
  /// content. Fails as the tree's status() and read() do, and with
  /// `is_a_directory` for a directory.
  [[nodiscard]] exec::task<
      std::expected<std::shared_ptr<const FileSnapshot>, std::error_code>>
  take(std::filesystem::path path) const;

  /// How many files have a snapshot that someone still holds.
  [[nodiscard]] std::size_t held() const;

 private:
  // The newest snapshot of each file, for as long as anyone holds it. Shared
  // with the snapshots themselves, whose deleters take them out of it, since a
  // snapshot may outlive this object.
  struct Current;
  class Forget;

  const DirectoryTree& m_tree;

  mutable std::mutex m_mutex;
  std::uint64_t m_clock = 0;
  // Every generation up to this was moved on by an "everything is dirty".
  std::uint64_t m_everything = 0;
  std::unordered_map<std::filesystem::path, std::uint64_t> m_generations;

  std::shared_ptr<Current> m_current;
};

}  // namespace makebelieve
