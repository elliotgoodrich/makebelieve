// SPDX-License-Identifier: MIT
#pragma once

#include "commandrunner.hpp"
#include "directorytree.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

struct sqlite3;

namespace makebelieve {

class NativeFile;

/// @class BuildCache
/// Keeps built outputs, and what each was built from, in a directory on disk,
/// so a later run can pick up where this one left off rather than rebuild
/// everything.
///
/// The directory holds `cache.sqlite`, one record per output - the command
/// that built it, its content's hash, when it was built, and a stamp of every
/// input it read - and `objects/`, one file per distinct content, named by its
/// hash. A content file is in place before any record names it, and a content
/// file that does not match its name reads as missing, so a crash at any point
/// leaves nothing that a later run would take for a good build. A content file
/// is never rewritten once in place, so whatever has one open - a reader of
/// the output it serves - goes on reading what it opened, even once no record
/// names it and it is deleted. Beside them is `scratch/`, for builds to write
/// to before what they built is stored, emptied whenever the cache opens.
///
/// Holds the directory exclusively for its whole life: a second `BuildCache`
/// on the same directory, in this process or another, fails to open.
///
/// Thread-safe: every member may be called from any thread.
class BuildCache {
 public:
  /// What an input looked like when the output that read it was built.
  struct Input {
    /// A source file or directory, relative to the source tree's root, or
    /// `@/` and the output that was read.
    std::filesystem::path path;

    /// For a source file or directory: its mtime as @link raw_mtime counts
    /// it, and its size - `-1` for a directory. Zero for an output.
    std::int64_t mtime = 0;
    std::int64_t size = 0;

    /// For an output: the hash of the content that was read. Empty for a
    /// source file or directory.
    std::string hash;

    [[nodiscard]] friend bool operator==(const Input&, const Input&) = default;
  };

  /// One output as it was last built.
  struct Record {
    /// The rule that built it.
    Command command;

    /// The hash of its content, which @link open opens.
    std::string hash;

    /// When it was built: the mtime it is served with.
    std::chrono::file_clock::time_point mtime;

    /// Every input its build read.
    std::vector<Input> inputs;

    [[nodiscard]] friend bool operator==(const Record&,
                                         const Record&) = default;
  };

  /// Opens the cache in @a directory, creating it if need be. A cache written
  /// by an incompatible version is emptied. Throws `std::system_error` if the
  /// directory cannot be created or the cache opened, with
  /// `device_or_resource_busy` if another `BuildCache` holds it.
  explicit BuildCache(std::filesystem::path directory);

  ~BuildCache();

  BuildCache(const BuildCache&) = delete;
  BuildCache& operator=(const BuildCache&) = delete;
  BuildCache(BuildCache&&) = delete;
  BuildCache& operator=(BuildCache&&) = delete;

  /// The directory this cache lives in.
  [[nodiscard]] const std::filesystem::path& directory() const;

  /// Where builds may write what they build before it is stored, on the same
  /// volume as the content, so moving it in is a rename. Emptied when the
  /// cache opens, so whatever a run leaves there is gone by the next.
  [[nodiscard]] const std::filesystem::path& scratch_directory() const;

  /// Every output recorded, in no particular order.
  [[nodiscard]] std::vector<Record> records() const;

  /// The content whose hash is @a hash, opened, or `no_such_file_or_directory`
  /// if it is missing or no longer matches its hash - which it reads it all
  /// through to find out. It goes on reading that content however the cache
  /// changes.
  [[nodiscard]] std::expected<std::shared_ptr<const NativeFile>,
                              std::error_code>
  open(std::string_view hash) const;

  /// Records @a command's output as @a bytes, built at @a mtime from
  /// @a inputs, replacing any earlier record of it. Returns the hash of
  /// @a bytes. Throws `std::system_error` if it cannot be stored, in which
  /// case the output is left unrecorded.
  std::string store(const Command& command,
                    std::string_view bytes,
                    std::chrono::file_clock::time_point mtime,
                    std::span<const Input> inputs);

  /// As @link store, but the content is the file @a file, whose hash is
  /// @a hash, which the cache takes over: it is moved into place, or deleted
  /// if that content is there already. Deleted too if it cannot be stored, in
  /// which case this throws `std::system_error` and the output is left
  /// unrecorded.
  /// @pre Nothing writes to @a file any more.
  void store_file(const Command& command,
                  const std::filesystem::path& file,
                  std::string_view hash,
                  std::chrono::file_clock::time_point mtime,
                  std::span<const Input> inputs);

  /// As @link store, but the content is what @a content reads, whose hash is
  /// @a hash - which is read through, a piece at a time, only if that content
  /// is not in the cache already.
  void store_content(const Command& command,
                     const OpenedFile& content,
                     std::string_view hash,
                     std::chrono::file_clock::time_point mtime,
                     std::span<const Input> inputs);

  /// Drops the records of @a outputs, and the content only they held. Outputs
  /// with no record are skipped. Throws `std::system_error` if it cannot.
  void forget(std::span<const std::filesystem::path> outputs);

  /// Deletes every content file that no record names, such as one a crash
  /// left behind. Best-effort: a file that cannot be deleted stays.
  void collect_garbage();

  /// The hash this cache names @a bytes by: 32 hex digits.
  [[nodiscard]] static std::string hash(std::string_view bytes);

  /// The hash of what @a file reads, read through a piece at a time, or the
  /// error that stopped it.
  [[nodiscard]] static std::expected<std::string, std::error_code> hash(
      const OpenedFile& file);

  /// @a time as an Input stores it.
  [[nodiscard]] static std::int64_t raw_mtime(
      std::chrono::file_clock::time_point time);

 private:
  std::filesystem::path m_directory;
  std::filesystem::path m_objects;
  std::filesystem::path m_scratch;

  // Serialises every use of the connection, and every change to the content
  // files with the records naming them.
  mutable std::mutex m_mutex;
  sqlite3* m_db = nullptr;

  // Numbers the scratch files content is written to before being renamed into
  // place, so two writes never share one.
  std::uint64_t m_next_scratch = 0;

  // Puts the content @a hash in place, unless it is there already, by having
  // @a write create it at a scratch path, then renaming that into place - so
  // the file under the hash's name never holds anything else. Throws
  // `std::system_error` if it cannot, leaving nothing behind.
  // @pre m_mutex is held.
  void place(const std::string& hash,
             const std::function<std::error_code(const std::filesystem::path&)>&
                 write);

  // Records @a command's output as the content @a hash, already in place,
  // replacing any earlier record - or, if that fails, deletes the content if
  // nothing else names it and throws.
  // @pre m_mutex is held.
  void record(const Command& command,
              const std::string& hash,
              std::chrono::file_clock::time_point mtime,
              std::span<const Input> inputs);

  // Deletes @a hash's content file if no record names it any more.
  // @pre m_mutex is held.
  void drop_if_unreferenced(const std::string& hash);
};

}  // namespace makebelieve
