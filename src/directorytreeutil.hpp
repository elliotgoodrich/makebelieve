// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace makebelieve {

class DirectoryTree;

/// @class DirectoryTreeUtil
/// Provides a namespace for DirectoryTree-related utility functions.
struct DirectoryTreeUtil {
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
