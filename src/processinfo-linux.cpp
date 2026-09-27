// SPDX-License-Identifier: MIT
#include "processinfo.hpp"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace makebelieve {

namespace {

// What `/proc/<id>/status` says about @a field, or nothing when the file or
// the field is missing - which is what a process that exited between the
// request and this lookup gives.
std::optional<std::string> read_proc_status(std::uint32_t id,
                                            std::string_view field) {
  std::ifstream status("/proc/" + std::to_string(id) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    if (!line.starts_with(field)) {
      continue;
    }
    const std::size_t start = line.find_first_not_of(" \t", field.size());
    if (start != std::string::npos) {
      return line.substr(start);
    }
  }
  return std::nullopt;
}

}  // namespace

std::uint32_t ProcessInfo::self() {
  return static_cast<std::uint32_t>(::getpid());
}

std::string ProcessInfo::name_of(std::uint32_t pid) {
  return read_proc_status(pid, "Name:").value_or("unknown");
}

}  // namespace makebelieve
