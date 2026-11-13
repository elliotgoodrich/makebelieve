// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <system_error>

namespace makebelieve {

/// @class OpenedFileUtil
/// Provides a namespace for opening files on disk as `OpenedFile`s.
struct OpenedFileUtil {
  /// Opens the file @a path for reading through a native handle, which no
  /// command launched meanwhile inherits, or says why it could not. Others
  /// may go on writing to it, or rename or delete it, while it is open:
  /// writes in place show through it, but a rename or a deletion leaves it
  /// reading the file it opened - so renaming a new file over its name, as a
  /// build that replaces a file does, leaves it reading the old one. Its
  /// `stat` gives its size and mtime as they are then, and a directory opens
  /// but fails `stat` with `is_a_directory`. A read runs on the thread that
  /// starts it.
  [[nodiscard]] static std::expected<std::shared_ptr<const OpenedFile>,
                                     std::error_code>
  open(const std::filesystem::path& path);
};

}  // namespace makebelieve
