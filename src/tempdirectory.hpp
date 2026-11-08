// SPDX-License-Identifier: MIT
#pragma once

#include <expected>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace makebelieve {

// Removes a directory and everything beneath it on destruction, so a command's
// scratch space does not outlive the build it was created for.
class TempDirectory {
  std::filesystem::path m_path;

 public:
  explicit TempDirectory(std::filesystem::path path)
      : m_path(std::move(path)) {}

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  TempDirectory(TempDirectory&&) = delete;
  TempDirectory& operator=(TempDirectory&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
};

// Creates a fresh directory under the system's temporary one, or says why it
// could not. Only a name that is taken already is retried.
inline std::expected<std::filesystem::path, std::error_code>
make_temp_directory(std::filesystem::path base = {}) {
  std::error_code ec;
  if (base.empty()) {
    base = std::filesystem::temp_directory_path(ec);
    if (ec) {
      return std::unexpected(ec);
    }
  }
  std::random_device device;
  std::uniform_int_distribution<unsigned> digit(0, 0xffffffU);
  constexpr int k_attempts = 100;
  for (int attempt = 0; attempt < k_attempts; ++attempt) {
    std::filesystem::path candidate =
        base / ("makebelieve-build-" + std::to_string(digit(device)));
    if (std::filesystem::create_directory(candidate, ec)) {
      return candidate;
    }
    // A directory already there is reported as nothing created; a file
    // there, as file_exists. Anything else will not go away by retrying.
    if (ec && ec != std::errc::file_exists) {
      return std::unexpected(ec);
    }
  }
  return std::unexpected(std::make_error_code(std::errc::file_exists));
}

}  // namespace makebelieve
