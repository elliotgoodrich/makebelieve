// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace makebelieve {

/// @class FileSystemUtil
/// Provides a namespace for filesystem-related functions.
struct FileSystemUtil {
  /// Returns whether @a a and @a b are disjoint. i.e. `a != b` and neither
  /// is a parent of the other.
  /// @pre Both paths are be absolute.
  [[nodiscard]] static bool are_disjoint(std::filesystem::path a,
                                         std::filesystem::path b);

  /// Returns the whole of the file @a path, or the error that stopped it. The
  /// file is opened so that no command launched meanwhile inherits it -
  /// unlike through `std::ifstream`, which cannot ask for that.
  [[nodiscard]] static std::expected<std::string, std::error_code> read_file(
      const std::filesystem::path& path);
};

}  // namespace makebelieve
