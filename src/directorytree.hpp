// SPDX-License-Identifier: MIT
#pragma once

#include "tracer.hpp"

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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

class OpenedFile;

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

/// The process requesting a file. Cancellation travels in the sender
/// environment.
struct OpenContext {
  /// Native process id of the requester; zero means no attributed process.
  std::uint32_t requester_pid = 0;

  /// The trace row the request is recorded on, if any, which a build the open
  /// starts draws its arrow from.
  std::optional<TraceLane> trace_lane;
};

/// A file operation yielding a value or filesystem error, an exception, or
/// stopped. Cancellation propagates through the receiver's stop token.
/// Implementations may use coroutines or ordinary sender composition.
/// No completion-thread affinity is promised, including for cancellation:
/// a sender may complete inside its stop callback. Consumers whose completion
/// code cannot run there must transfer completion to a suitable scheduler.
template <class T>
using FileSender = exec::any_sender<exec::any_receiver<
    stdexec::completion_signatures<stdexec::set_value_t(
                                       std::expected<T, std::error_code>),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>,
    exec::queries<stdexec::inplace_stop_token(
        stdexec::get_stop_token_t) noexcept>>>;
/// An asynchronous open yielding a file handle or an error.
using OpenSender = FileSender<std::shared_ptr<const OpenedFile>>;
/// An asynchronous read yielding file bytes or an error.
using ReadSender = FileSender<std::string>;

/// A handle to the file that was opened. If another file replaces it at the
/// same path, stat() and read() still refer to the original file. Its contents
/// may change while the handle is open. Keep the handle alive until each read
/// sender has finished.
class OpenedFile {
 public:
  virtual ~OpenedFile() = default;
  [[nodiscard]] virtual std::expected<FileInfo, std::error_code> stat()
      const = 0;
  /// Reads up to size bytes at offset. A short read indicates EOF.
  [[nodiscard]] virtual ReadSender read(Offset offset,
                                        std::size_t size) const = 0;
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

  /// Opens @a path and retains its file identity for the life of the handle.
  /// Returns `is_a_directory` for a directory. @a context identifies the
  /// requester. Cancellation
  /// detaches this reader without stopping a build shared by other readers.
  /// Completion follows the @link FileSender threading contract.
  [[nodiscard]] virtual OpenSender open(
      const std::filesystem::path& path,
      const OpenContext& context = {}) const = 0;

  /// Subscribes to changes in this object by calling @a callback and returns an
  /// RAII guard that will unsubscribe on destruction.
  /// @note Paths in the diff are relative to the tree root.
  /// @pre The returned object does not outlive this `DirectoryTree`.
  [[nodiscard]] virtual Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) const = 0;
};

}  // namespace makebelieve
