// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

#include <string>

namespace makebelieve {

class DirectoryTree;

/// @class DirectoryTreeUtil
/// Provides a namespace for DirectoryTree-related utility functions.
struct DirectoryTreeUtil {
  /// Returns a sender that calls @a tree's status for @a path when started,
  /// yielding file metadata or `is_a_directory` for a directory.
  /// For trees whose files are already final; does not start a build.
  /// @pre @a tree outlives the operation.
  [[nodiscard]] static OpenSender open_by_status(
      const DirectoryTree& tree,
      const std::filesystem::path& path);

  /// Blocks until @a tree opens @a path for @a context. Maps stopped to
  /// `operation_canceled` and rethrows sender exceptions. Supplies no stop
  /// request; must not run on a dispatcher or executor needed by the open.
  [[nodiscard]] static std::expected<FileInfo, std::error_code> open(
      const DirectoryTree& tree,
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

  /// Returns whether @a a and @a b are equal, per @link diff.
  [[nodiscard]] static bool equal(const DirectoryTree& a,
                                  const DirectoryTree& b);
};

}  // namespace makebelieve
