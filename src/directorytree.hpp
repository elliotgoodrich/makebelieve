// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#ifndef WIN32
#include <sys/types.h>  // off_t
#else
#include <basetsd.h>  // INT64
#endif

namespace makebelieve {

/// \class Offset
/// Offset index into a file.
#ifdef WIN32
using Offset = INT64;
#else
using Offset = off_t;
#endif

/// \class FileInfo
/// Describes a file in a DirectoryTree.
struct FileInfo {
  std::size_t size;
  std::chrono::file_clock::time_point mtime;
};

/// \class DirectoryInfo
/// Describes a directory in a DirectoryTree.
struct DirectoryInfo {
  std::chrono::file_clock::time_point mtime;
};

/// \class EntryInfo
/// Describes a file or directory in a DirectoryTree.
using EntryInfo = std::variant<FileInfo, DirectoryInfo>;

/// \class TreeEntry
/// Describes a directory entry in a DirectoryTree - either a file or a
/// directory.
class TreeEntry {
 public:
  std::filesystem::path name;
  EntryInfo info;
};

/// \class DirectoryTreeDiff
/// Describes a change in between states of a DirectoryTree.  Paths are
/// relative to the tree root; the root itself is the empty path.
class DirectoryTreeDiff {
 public:
  /// Set when the tree lost track of what changed and everything could
  /// be dirty.  If this is true then the below entries are empty.
  bool everything_dirty = false;

  /// Paths whose own identity changed: files added, written or deleted,
  /// and directories added or deleted. A cached status() or read() for any
  /// of these is stale.
  std::vector<std::filesystem::path> entries_changed;

  /// Directories whose set of children changed. A cached ls() for any of
  /// these is stale.
  ///
  /// This also implies the directory's own status() is stale, since adding
  /// or removing an entry moves the directory's mtime. Those directories
  /// are deliberately not repeated in entries_changed.
  std::vector<std::filesystem::path> child_lists_changed;
};

/// \class Subscription
/// RAII guard that unsubscribes on destruction.
class Subscription {
  std::move_only_function<void()> m_unsubscribe;

 public:
  /// Creates a `Subscription` object that will call @a unsubscribe
  /// on destruction if `(bool)unsubscribe`.
  explicit Subscription(std::move_only_function<void()> unsubscribe)
      : m_unsubscribe(std::move(unsubscribe)) {}

  /// Calls the held unsubscribe function if it it comparable to true.
  ~Subscription() {
    if (m_unsubscribe) {
      m_unsubscribe();
    }
  }

  Subscription(Subscription&& other) noexcept
      : m_unsubscribe(std::exchange(other.m_unsubscribe, nullptr)) {}

  Subscription& operator=(Subscription other) noexcept {
    using std::swap;
    swap(m_unsubscribe, other.m_unsubscribe);
    return *this;
  }

  Subscription(const Subscription&) = delete;
};

/// @class DirectoryTree
/// An abstract base class to describe an observable directory tree.
class DirectoryTree {
 public:
  virtual ~DirectoryTree() = default;

  /// Returns entry at @a path, or an error_code when it is missing,
  /// unreachable, or escapes the root.
  [[nodiscard]] virtual std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const = 0;

  /// Returns the children of @a path, or an error_code when it cannot be
  /// listed. An existing but empty directory yields an empty vector.
  [[nodiscard]] virtual std::expected<std::vector<TreeEntry>, std::error_code>
  ls(const std::filesystem::path& path) const = 0;

  /// Returns up to @a size bytes of @a path starting at @a offset, or an
  /// error_code on failure. A result shorter than @a size means end of file. A
  /// @a size of 0 succeeds with an empty result without opening @a path.
  [[nodiscard]] virtual std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size) const = 0;

  /// Subscribes to changes in this object by calling @a callback and returns an
  /// RAII guard that will unsubscribe on destruction.
  /// @note Paths in the diff are relative to the tree root.
  /// @pre The returned object does not outlive this `DirectoryTree`.
  [[nodiscard]] virtual Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) const = 0;
};

}  // namespace makebelieve
