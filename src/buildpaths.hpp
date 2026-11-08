// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace makebelieve {

// Canonical absolute paths owned by one mount. Notification names are compared
// lexically: inspecting the virtual filesystem from its watcher can itself
// issue a filesystem request.
struct BuildPaths {
  std::filesystem::path mount;
  std::filesystem::path cache;
  // Included in the source tree's exclusions when scratch is inside it.
  // Build processes themselves use this directory for %out and @/ inputs.
  std::filesystem::path scratch{};

  static bool within(const std::filesystem::path& path,
                     const std::filesystem::path& directory) {
    if (directory.empty()) {
      return false;
    }
    const auto equal = [](const auto& a, const auto& b) {
#ifdef _WIN32
      return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) ==
             CSTR_EQUAL;
#else
      return a == b;
#endif
    };
    return std::ranges::mismatch(directory, path, equal).in1 == directory.end();
  }

  [[nodiscard]] bool contains(const std::filesystem::path& path) const {
    return within(path, mount) || within(path, cache) || within(path, scratch);
  }
  // Source queries and built-in copies can follow filesystem links. Events
  // use contains() directly because they name the entry that changed.
  [[nodiscard]] bool contains_resolved(
      const std::filesystem::path& path) const {
    if (mount.empty() && cache.empty() && scratch.empty()) {
      return false;
    }
    if (contains(path.lexically_normal())) {
      return true;
    }
    std::error_code ec;
    return contains(std::filesystem::weakly_canonical(path, ec));
  }
};

}  // namespace makebelieve
