// SPDX-License-Identifier: MIT
#include "processinfo.hpp"

#include "stringutil.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace makebelieve {

namespace {

// The file name of the executable the process @a id is running, or nothing
// when it has exited or may not be queried.
std::optional<std::string> executable_name(std::uint32_t id) {
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
  if (process == nullptr) {
    return std::nullopt;
  }
  std::array<wchar_t, MAX_PATH> buffer{};
  auto size = static_cast<DWORD>(buffer.size());
  const BOOL queried =
      QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
  CloseHandle(process);
  if (queried == 0) {
    return std::nullopt;
  }
  return StringUtil::to_utf8(
      std::filesystem::path(std::wstring_view(buffer.data(), size))
          .filename()
          .wstring());
}

}  // namespace

std::uint32_t ProcessInfo::self() {
  return GetCurrentProcessId();
}

std::string ProcessInfo::name_of(std::uint32_t pid) {
  return executable_name(pid).value_or("unknown");
}

}  // namespace makebelieve
