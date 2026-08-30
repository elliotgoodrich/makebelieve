// SPDX-License-Identifier: MIT
#include "stringutil.hpp"

#ifdef _WIN32

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

#include <windows.h>

namespace makebelieve {

std::string StringUtil::to_utf8(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                      static_cast<int>(wide.size()), nullptr, 0,
                                      nullptr, nullptr);
  if (len <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      result.data(), len, nullptr, nullptr);
  return result;
}

std::expected<std::wstring, std::error_code> StringUtil::to_wide(
    std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length == 0) {
    return std::unexpected(std::error_code(static_cast<int>(GetLastError()),
                                           std::system_category()));
  }
  // TODO: Use resize_and_overwrite
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), length);
  return wide;
}

}  // namespace makebelieve

#endif  // _WIN32
