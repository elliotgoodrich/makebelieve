// SPDX-License-Identifier: MIT
#include "filesystemutil.hpp"

#include "scopeguard.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace makebelieve {

bool FileSystemUtil::are_disjoint(std::filesystem::path a,
                                  std::filesystem::path b) {
  assert(a.is_absolute());
  assert(b.is_absolute());
  a = std::filesystem::weakly_canonical(a);
  b = std::filesystem::weakly_canonical(b);
  const auto [a_it, b_it] = std::ranges::mismatch(a, b);
  // If either range is exhausted it is a prefix of the other - an ancestor of,
  // or equal to, it - so the two overlap. They are disjoint only when both
  // still have a component left, i.e. they diverged before either ran out.
  return a_it != a.end() && b_it != b.end();
}

std::expected<std::string, std::error_code> FileSystemUtil::read_file(
    const std::filesystem::path& path) {
  std::string contents;
  std::array<char, 4096> buffer{};
#ifdef _WIN32
  // Not inheritable, as no SECURITY_ATTRIBUTES asks for it to be.
  const HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::unexpected(std::error_code(static_cast<int>(GetLastError()),
                                           std::system_category()));
  }
  const ScopeGuard close_file([file] { CloseHandle(file); });
  while (true) {
    DWORD n = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &n,
                  nullptr)) {
      return std::unexpected(std::error_code(static_cast<int>(GetLastError()),
                                             std::system_category()));
    }
    if (n == 0) {
      break;
    }
    contents.append(buffer.data(), n);
  }
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  const ScopeGuard close_file([fd] { ::close(fd); });
  // To the end, rather than to a size, which /proc files report as 0.
  while (true) {
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(std::error_code(errno, std::system_category()));
    }
    contents.append(buffer.data(), static_cast<std::size_t>(n));
  }
#endif
  return contents;
}

namespace {

// A file created, or truncated, for writing, which no command launched
// meanwhile inherits - or the error that stopped that.
class OutputFile {
#ifdef _WIN32
  HANDLE m_file;
#else
  int m_file;
#endif
  std::error_code m_error;

 public:
  explicit OutputFile(const std::filesystem::path& path)
#ifdef _WIN32
      // Not inheritable, as no SECURITY_ATTRIBUTES asks for it to be.
      : m_file(CreateFileW(path.c_str(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr)) {
    if (m_file == INVALID_HANDLE_VALUE) {
      m_error = {static_cast<int>(GetLastError()), std::system_category()};
    }
  }
#else
      : m_file(::open(path.c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      0666)) {
    if (m_file < 0) {
      m_error = {errno, std::system_category()};
    }
  }
#endif

  ~OutputFile() {
    if (!m_error) {
#ifdef _WIN32
      CloseHandle(m_file);
#else
      ::close(m_file);
#endif
    }
  }

  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;
  OutputFile(OutputFile&&) = delete;
  OutputFile& operator=(OutputFile&&) = delete;

  // Why the file could not be opened, if it could not.
  [[nodiscard]] const std::error_code& error() const { return m_error; }

  // Writes the whole of @a contents, or says why it could not.
  [[nodiscard]] std::error_code write(std::string_view contents) const {
    while (!contents.empty()) {
#ifdef _WIN32
      // WriteFile takes at most a DWORD's worth at a time.
      const DWORD chunk = static_cast<DWORD>(
          std::min<std::size_t>(contents.size(), DWORD{1} << 30U));
      DWORD n = 0;
      if (!WriteFile(m_file, contents.data(), chunk, &n, nullptr)) {
        return {static_cast<int>(GetLastError()), std::system_category()};
      }
      contents.remove_prefix(n);
#else
      const ssize_t n = ::write(m_file, contents.data(), contents.size());
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return {errno, std::system_category()};
      }
      contents.remove_prefix(static_cast<std::size_t>(n));
#endif
    }
    return {};
  }
};

}  // namespace

std::error_code FileSystemUtil::write_file(const std::filesystem::path& path,
                                           std::string_view contents) {
  const OutputFile file(path);
  return file.error() ? file.error() : file.write(contents);
}

std::error_code FileSystemUtil::write_file(
    const std::filesystem::path& path,
    const std::function<std::expected<std::string, std::error_code>()>& next) {
  const OutputFile file(path);
  if (file.error()) {
    return file.error();
  }
  while (true) {
    const std::expected<std::string, std::error_code> piece = next();
    if (!piece.has_value()) {
      return piece.error();
    }
    if (piece->empty()) {
      return {};
    }
    if (const std::error_code error = file.write(*piece)) {
      return error;
    }
  }
}

std::error_code FileSystemUtil::hide(const std::filesystem::path& path) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return {static_cast<int>(GetLastError()), std::system_category()};
  }
  if ((attributes & FILE_ATTRIBUTE_HIDDEN) == 0 &&
      !SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN)) {
    return {static_cast<int>(GetLastError()), std::system_category()};
  }
  return {};
#else
  // There is no attribute to set: a name starting with `.` is what hides it.
  std::error_code error;
  if (!std::filesystem::exists(path, error) && !error) {
    error = std::make_error_code(std::errc::no_such_file_or_directory);
  }
  return error;
#endif
}

}  // namespace makebelieve
