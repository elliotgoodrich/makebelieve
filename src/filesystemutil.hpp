// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>

namespace makebelieve {

/// @class FileSystemUtil
/// Provides a namespace for filesystem-related functions.
struct FileSystemUtil {
  /// Returns whether @a a and @a b are disjoint. i.e. `a != b` and neither
  /// is a parent of the other.
  /// @pre Both paths are be absolute.
  [[nodiscard]] static bool are_disjoint(std::filesystem::path a,
                                         std::filesystem::path b);
};

}  // namespace makebelieve
