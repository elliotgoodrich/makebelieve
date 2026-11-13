// SPDX-License-Identifier: MIT
#include "nativefile.hpp"

#include "scopeguard.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <windows.h>

namespace makebelieve {

namespace {

// ReadFile takes a DWORD count, so a request larger than 4GiB has to be read
// across several calls. Capping well below the DWORD limit also keeps a
// single call from pinning an unreasonable span of the output buffer.
constexpr std::uint64_t k_read_chunk_bytes = 32ULL * 1024 * 1024;

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

// Splices a Win32 high/low pair into one 64-bit value.
std::uint64_t combine(DWORD high, DWORD low) {
  return (static_cast<std::uint64_t>(high) << 32) | low;
}

// Converts a FILETIME into a `file_clock::time_point`.
std::chrono::file_clock::time_point to_file_time(const FILETIME& time) {
  // MSVC's file_clock counts 100ns ticks from the Windows epoch (1601-01-01),
  // which is exactly FILETIME's representation, so this is a reinterpretation
  // rather than a conversion.
  return std::chrono::file_clock::time_point(
      std::chrono::file_clock::duration(static_cast<std::int64_t>(
          combine(time.dwHighDateTime, time.dwLowDateTime))));
}

// Reads up to @a size bytes of @a file at @a offset, as OpenedFile::read does.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): as OpenedFile::read
std::expected<std::string, std::error_code> read_at(HANDLE file,
                                                    Offset offset,
                                                    std::size_t size) {
  if (offset < 0) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }
  if (size == 0) {
    return std::string{};
  }
  std::string contents;
  std::optional<std::error_code> failure;
  contents.resize_and_overwrite(size, [&](char* const data,
                                          const std::size_t capacity) noexcept {
    std::size_t total = 0;
    while (total < capacity) {
      // The handle is synchronous, so passing an OVERLAPPED here does not
      // make the read asynchronous - it just supplies the file position,
      // which is what lets concurrent reads of the same file share no state.
      const std::uint64_t position = static_cast<std::uint64_t>(offset) + total;
      OVERLAPPED overlapped{};
      overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
      overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);

      const DWORD chunk = static_cast<DWORD>(
          std::min<std::uint64_t>(capacity - total, k_read_chunk_bytes));
      DWORD read_bytes = 0;
      if (!ReadFile(file, data + total, chunk, &read_bytes, &overlapped)) {
        // A positioned read that starts at or past the end reports itself
        // this way rather than as a zero-byte success.
        if (GetLastError() != ERROR_HANDLE_EOF) {
          failure = last_error_code();
        }
        break;
      }
      if (read_bytes == 0) {
        break;
      }
      total += read_bytes;
    }
    return total;
  });

  if (failure.has_value()) {
    return std::unexpected(*failure);
  }
  return contents;
}

}  // namespace

std::expected<std::shared_ptr<const NativeFile>, std::error_code>
NativeFile::open(const std::filesystem::path& path) {
  // Not inheritable, as no SECURITY_ATTRIBUTES asks for it to be. Writers may
  // edit, rename or delete the file while it is held; backup semantics let a
  // directory open too, for stat() to say what it is.
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::unexpected(last_error_code());
  }
  ScopeGuard close_file([file] { CloseHandle(file); });
  auto opened = std::make_shared<const NativeFile>(Private{}, file);
  close_file.dismiss();
  return opened;
}

NativeFile::~NativeFile() {
  CloseHandle(m_handle);
}

std::expected<FileInfo, std::error_code> NativeFile::stat() const {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(m_handle, &info)) {
    return std::unexpected(last_error_code());
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return std::unexpected(std::make_error_code(std::errc::is_a_directory));
  }
  return FileInfo{.size = static_cast<std::size_t>(
                      combine(info.nFileSizeHigh, info.nFileSizeLow)),
                  .mtime = to_file_time(info.ftLastWriteTime)};
}

ReadSender NativeFile::read(Offset offset, std::size_t size) const {
  return stdexec::just(m_handle, offset, size) | stdexec::then(read_at);
}

}  // namespace makebelieve
