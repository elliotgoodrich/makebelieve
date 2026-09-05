// SPDX-License-Identifier: MIT
#include "unmountchannel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

namespace makebelieve {

namespace {

// FNV-1a over a byte range. Small and deterministic, unlike std::hash which the
// standard permits to vary between executions.
std::uint64_t fnv1a(const void* data, std::size_t size) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

}  // namespace

std::string UnmountChannel::endpoint_id(const std::filesystem::path& key) {
  // Hash the path's native bytes, whose width - wchar_t on Windows, char on
  // Linux - the sizeof accounts for.
  const auto& native = key.native();
  const std::uint64_t hash = fnv1a(
      native.data(), native.size() * sizeof(std::filesystem::path::value_type));

  // Format the 16 hex digits straight into the string's buffer. The 17th char
  // is scratch for snprintf's terminator; returning 16 trims it back off.
  std::string id;
  id.resize_and_overwrite(17, [hash](char* buffer, std::size_t size) {
    std::snprintf(buffer, size, "%016llx",
                  static_cast<unsigned long long>(hash));
    return std::size_t{16};
  });
  return id;
}

bool UnmountChannel::wait_until_gone(const std::filesystem::path& mountpoint) {
  const std::filesystem::path absolute = std::filesystem::absolute(mountpoint);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code error;
    if (!std::filesystem::exists(absolute, error)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace makebelieve
