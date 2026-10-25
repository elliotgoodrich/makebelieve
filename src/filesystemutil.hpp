// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
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

  /// Creates or truncates the file @a path and writes @a contents to it,
  /// returning the error that stopped it, if any. As with read_file, no
  /// command launched meanwhile inherits the file.
  [[nodiscard]] static std::error_code write_file(
      const std::filesystem::path& path,
      std::string_view contents);
};

}  // namespace makebelieve
