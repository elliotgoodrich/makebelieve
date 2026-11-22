// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
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

  /// As above, but writing what @a next gives, a piece at a time, until it
  /// gives an empty piece - or an error, which is returned.
  [[nodiscard]] static std::error_code write_file(
      const std::filesystem::path& path,
      const std::function<std::expected<std::string, std::error_code>()>& next);

  /// @a path in the form to compare with others to tell whether they name the
  /// same thing as a filesystem would: as it is on Linux, and with its case
  /// folded on Windows, where a directory usually ignores case. So on Windows
  /// two paths in a case-sensitive directory can share a key, which only
  /// costs whatever keeps them the work of treating them as one.
  [[nodiscard]] static std::filesystem::path path_key(
      const std::filesystem::path& path);

  /// Attempts to hide the existing file or directory @a path from listings
  /// that skip hidden entries, returning the error that stopped it, if any.
  [[nodiscard]] static std::error_code hide(const std::filesystem::path& path);
};

}  // namespace makebelieve
