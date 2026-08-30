// SPDX-License-Identifier: MIT
#pragma once

#ifdef _WIN32

#include <expected>
#include <string>
#include <string_view>
#include <system_error>

namespace makebelieve {

/// @class StringUtil
/// Provides a namespace for the Win32 conversions between the UTF-16 the wide
/// Windows API speaks and the UTF-8 the rest of the code uses. Windows-only.
struct StringUtil {
  /// Narrows UTF-16 @a wide to UTF-8. Best-effort: an empty or unconvertible
  /// input yields an empty string.
  [[nodiscard]] static std::string to_utf8(std::wstring_view wide);

  /// Widens UTF-8 @a text to UTF-16, or an `error_code` when the conversion
  /// fails. An empty input yields an empty string.
  [[nodiscard]] static std::expected<std::wstring, std::error_code> to_wide(
      std::string_view text);
};

}  // namespace makebelieve

#endif  // _WIN32
