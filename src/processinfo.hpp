// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace makebelieve {

/// @class ProcessInfo
/// Provides a namespace for asking about processes: this one, and those that
/// reach the filesystem it serves.
struct ProcessInfo {
  /// This process's own id.
  [[nodiscard]] static std::uint32_t self();

  /// The name of the executable the process @a pid is running, or `unknown`
  /// where it has exited or this process may not query it.
  [[nodiscard]] static std::string name_of(std::uint32_t pid);
};

}  // namespace makebelieve
