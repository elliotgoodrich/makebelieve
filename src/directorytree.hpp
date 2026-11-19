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
#include <map>
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
///
/// The value of a file is its content, and the value of a directory is its
/// entries: the name of each file and directory directly in it. Nothing else
/// about either - a directory's own modification time or attributes, say -
/// is part of its value. A diff names each path whose value changed, and why:
///
/// | Change             | added       | removed  | modified | entries_changed |
/// |--------------------|-------------|----------|----------|-----------------|
/// | file created       | it          |          |          | its parent      |
/// | file written       |             |          | it       |                 |
/// | file deleted       |             | it       |          | its parent      |
/// | file renamed/moved | new path    | old path |          | both parents    |
/// | dir made/moved in  | it, all in  |          |          | its parent      |
/// | dir gone/moved out |             | it       |          | its parent      |
/// | dir renamed/moved  | new, all in | old path |          | both parents    |
/// | dir mtime/attrs    |             |          |          |                 |
///
/// "All in" is every file and directory beneath the directory, which has
/// appeared with it. A path that replaces another of the other kind - a
/// directory where a file was - is removed, as what was there went.
///
/// A directory removed stands for everything that was beneath it too, which
/// once it has gone can no longer be named one by one. So a consumer treats
/// a directory removed as everything beneath it removed.
///
/// False positives are allowed: a path may be named that did not really
/// change, such as a file rewritten with the same bytes, or whose
/// modification time alone moved, since telling those apart would mean
/// reading it. They cost the consumer work, so are best avoided where that
/// is cheap. A change that is not reported is never allowed.
class DirectoryTreeDiff {
 public:
  /// Why a path is named.
  enum class Reason : std::uint8_t {
    /// A directory's entries changed where it is: one came or went.
    entries_changed,

    /// A file's content changed where it is.
    modified,

    /// It appeared: created, moved in, or the new name of a rename.
    added,

    /// It went: deleted, moved out, or the old name of a rename - and, for a
    /// directory, everything that was beneath it with it.
    removed,
  };

  /// Set when the tree lost track of what changed and everything could
  /// be dirty.  If this is true then `changes` is empty.
  bool everything_dirty = false;

  /// Every path whose value changed, as the table above says, and why. A
  /// cached status(), read() or ls() of any of these is stale.
  std::map<std::filesystem::path, Reason> changes;

  /// Names @a path as changed for @a reason. A path named for more than one
  /// reason keeps the one that says the most: removed, then added, then
  /// modified, then entries_changed. So a path that went and came back in
  /// one diff is removed, which says that whatever was beneath it may have
  /// gone.
  void note(const std::filesystem::path& path, Reason reason) {
    const auto [it, inserted] = changes.try_emplace(path, reason);
    if (!inserted && reason > it->second) {
      it->second = reason;
    }
  }
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
  ///
  /// @a context identifies the requester, as for open(). A tree that depends
  /// on who looks at what, as a `BuildDirectoryTree` does, takes a look on
  /// behalf of a requester as one of what that requester depends on, which
  /// may read `/proc` to find out who it is - so call it for a requester
  /// holding no lock. A look on nobody's behalf, such as one a tree or a
  /// mount makes for its own ends, passes no requester.
  [[nodiscard]] virtual std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path,
      const OpenContext& context = {}) const = 0;

  /// Returns the children of @a path, or an error_code when it cannot be
  /// listed. An existing but empty directory yields an empty vector.
  /// @a context is as for status(): a listing for a requester depends on the
  /// directory's entries.
  [[nodiscard]] virtual std::expected<std::vector<TreeEntry>, std::error_code>
  ls(const std::filesystem::path& path,
     const OpenContext& context = {}) const = 0;

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
