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
#include <string>
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

}  // namespace makebelieve
