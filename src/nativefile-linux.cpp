// SPDX-License-Identifier: MIT
#include "nativefile.hpp"

#include "scopeguard.hpp"

#include <stdexec/execution.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {errno, std::system_category()};
}

// Converts a POSIX timestamp to the file_clock time_point FileInfo carries.
std::chrono::file_clock::time_point to_file_time(const struct timespec& time) {
  const auto system_time = std::chrono::system_clock::from_time_t(time.tv_sec) +
                           std::chrono::nanoseconds(time.tv_nsec);
  return std::chrono::clock_cast<std::chrono::file_clock>(system_time);
}

// Reads up to @a size bytes of @a file at @a offset, as OpenedFile::read does.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): as OpenedFile::read
std::expected<std::string, std::error_code> read_at(int file,
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
  contents.resize_and_overwrite(size, [&](char* data,
                                          const std::size_t capacity) noexcept {
    std::size_t total = 0;
    while (total < capacity) {
      if (total > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max() -
                                             offset)) {
        failure = std::make_error_code(std::errc::value_too_large);
        break;
      }
      const ssize_t read_bytes =
          ::pread(file, data + total, capacity - total,
                  static_cast<off_t>(offset) + static_cast<off_t>(total));
      if (read_bytes < 0) {
        if (errno == EINTR) {
          continue;
        }
        failure = last_error_code();
        break;
      }
      if (read_bytes == 0) {
        break;  // End of file: a short result is how the contract spells EOF.
      }
      total += static_cast<std::size_t>(read_bytes);
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
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::unexpected(last_error_code());
  }
  ScopeGuard close_file([fd] { ::close(fd); });
  auto opened = std::make_shared<const NativeFile>(Private{}, fd);
  close_file.dismiss();
  return opened;
}

NativeFile::~NativeFile() {
  ::close(m_handle);
}

std::expected<FileInfo, std::error_code> NativeFile::stat() const {
  struct stat info {};
  if (::fstat(m_handle, &info) < 0) {
    return std::unexpected(last_error_code());
  }
  if (S_ISDIR(info.st_mode)) {
    return std::unexpected(std::make_error_code(std::errc::is_a_directory));
  }
  return FileInfo{.size = static_cast<std::size_t>(info.st_size),
                  .mtime = to_file_time(info.st_mtim)};
}

ReadSender NativeFile::read(Offset offset, std::size_t size) const {
  return stdexec::just(m_handle, offset, size) | stdexec::then(read_at);
}

}  // namespace makebelieve
