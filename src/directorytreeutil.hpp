// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace makebelieve {

class DirectoryTree;

/// @class DirectoryTreeUtil
/// Provides a namespace for DirectoryTree-related utility functions.
struct DirectoryTreeUtil {
  /// Blocks until @a tree opens @a path for @a context. Maps stopped to
  /// `operation_canceled` and rethrows sender exceptions. Supplies no stop
  /// request; must not run on a dispatcher or executor needed by the open.
  [[nodiscard]] static std::expected<std::shared_ptr<const OpenedFile>,
                                     std::error_code>
  open(const DirectoryTree& tree,
       const std::filesystem::path& path,
       const OpenContext& context = {});
  /// Blocks until @a tree reads up to @a size bytes from @a path at @a offset.
  /// Maps stopped to `operation_canceled` and rethrows sender exceptions.
  /// Supplies no stop request; must not run on an executor needed by the read.
  [[nodiscard]] static std::expected<std::string, std::error_code> read(
      const DirectoryTree& tree,
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size);

  /// Opens the file and reads the requested range.
  [[nodiscard]] static ReadSender read_range(const DirectoryTree& tree,
                                             const std::filesystem::path& path,
                                             Offset offset,
                                             std::size_t size);

  /// Opens once and reads to EOF. Only callers needing the whole file pay
  /// for materializing it. Changes made in place follow the handle's semantics.
  [[nodiscard]] static ReadSender read_whole(const DirectoryTree& tree,
                                             const std::filesystem::path& path,
                                             const OpenContext& context = {});
  /// Blocks for a read sender, mapping stopped to operation_canceled.
  [[nodiscard]] static std::expected<std::string, std::error_code> wait_read(
      ReadSender sender);

  /// Creates or truncates the file @a path and writes into it all that
  /// @a file reads, a piece at a time, returning the error that stopped it,
  /// if any. Blocks on each read, as @link wait_read does. No command launched
  /// meanwhile inherits the file written.
  [[nodiscard]] static std::error_code write_file(
      const std::filesystem::path& path,
      const OpenedFile& file);

  /// Compares @a a and @a b for structural equality: every path reachable
  /// from the root has the same kind (file or directory) in both, the same
  /// content for a file, and the same set of children for a directory.
  /// mtimes and subscribe_to_changes() behaviour are not compared.
  ///
  /// Returns an empty string when equal, or a message describing the first
  /// difference found - meant to be used directly as a test failure
  /// message, e.g. `EXPECT_TRUE(diff.empty()) << diff;`.
  [[nodiscard]] static std::string diff(const DirectoryTree& a,
                                        const DirectoryTree& b);

  /// Maps a tree-relative @a path onto its normalized, root-relative form -
  /// the root itself being the empty path - or nothing when it escapes the
  /// root. Purely lexical, as RealDirectoryTree is: no entry needs to exist.
  [[nodiscard]] static std::optional<std::filesystem::path> normalize(
      const std::filesystem::path& path);

  /// Whether the normalized @a entry is strictly beneath the normalized
  /// @a directory, not equal to it.
  [[nodiscard]] static bool is_within(const std::filesystem::path& entry,
                                      const std::filesystem::path& directory);

  /// An opened file holding @a bytes itself, with @a mtime: what a tree held
  /// in memory hands out. Opens sharing @a bytes share no other state.
  [[nodiscard]] static std::shared_ptr<const OpenedFile> memory_file(
      std::shared_ptr<const std::string> bytes,
      std::chrono::file_clock::time_point mtime);

  /// Returns whether @a a and @a b are equal, per @link diff.
  [[nodiscard]] static bool equal(const DirectoryTree& a,
                                  const DirectoryTree& b);
};

}  // namespace makebelieve
