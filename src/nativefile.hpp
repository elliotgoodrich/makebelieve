// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <system_error>

namespace makebelieve {

/// @class NativeFile
/// A file on disk opened for reading through a native handle, which no
/// command launched meanwhile inherits. Others may go on writing to it, or
/// rename or delete it, while it is open: writes in place show through it,
/// but a rename or a deletion leaves it reading the file it opened - so
/// renaming a new file over its name, as a build that replaces a file does,
/// leaves it reading the old one.
class NativeFile final : public OpenedFile {
#ifdef _WIN32
  void* m_handle;
#else
  int m_handle;
#endif

  struct Private {};

 public:
  /// Opens the file @a path, or says why it could not. A directory opens, but
  /// its `stat` fails with `is_a_directory`.
  [[nodiscard]] static std::expected<std::shared_ptr<const NativeFile>,
                                     std::error_code>
  open(const std::filesystem::path& path);

#ifdef _WIN32
  NativeFile(Private, void* handle) : m_handle(handle) {}
#else
  NativeFile(Private, int handle) : m_handle(handle) {}
#endif
  ~NativeFile() override;

  NativeFile(const NativeFile&) = delete;
  NativeFile& operator=(const NativeFile&) = delete;
  NativeFile(NativeFile&&) = delete;
  NativeFile& operator=(NativeFile&&) = delete;

  /// Its size and mtime as they are now.
  [[nodiscard]] std::expected<FileInfo, std::error_code> stat() const override;

  /// Reads up to @a size bytes at @a offset, on the thread that starts it.
  [[nodiscard]] ReadSender read(Offset offset, std::size_t size) const override;
};

}  // namespace makebelieve
