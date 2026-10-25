// SPDX-License-Identifier: MIT
#pragma once

#include "commandrunner.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

struct sqlite3;

namespace makebelieve {

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
/// leaves nothing that a later run would take for a good build.
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

    /// The hash of its content, which @link read hands back.
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

  /// Every output recorded, in no particular order.
  [[nodiscard]] std::vector<Record> records() const;

  /// The content whose hash is @a hash, or `no_such_file_or_directory` if it
  /// is missing or no longer matches its hash.
  [[nodiscard]] std::expected<std::string, std::error_code> read(
      std::string_view hash) const;

  /// Records @a command's output as @a bytes, built at @a mtime from
  /// @a inputs, replacing any earlier record of it. Returns the hash of
  /// @a bytes. Throws `std::system_error` if it cannot be stored, in which
  /// case the output is left unrecorded.
  std::string store(const Command& command,
                    std::string_view bytes,
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

  /// @a time as an Input stores it.
  [[nodiscard]] static std::int64_t raw_mtime(
      std::chrono::file_clock::time_point time);

 private:
  std::filesystem::path m_directory;
  std::filesystem::path m_objects;

  // Serialises every use of the connection, and every change to the content
  // files with the records naming them.
  mutable std::mutex m_mutex;
  sqlite3* m_db = nullptr;

  // Numbers the scratch files content is written to before being renamed into
  // place, so two writes never share one.
  std::uint64_t m_next_scratch = 0;

  // Deletes @a hash's content file if no record names it any more.
  // @pre m_mutex is held.
  void drop_if_unreferenced(const std::string& hash);
};

}  // namespace makebelieve
