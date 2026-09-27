// SPDX-License-Identifier: MIT
#pragma once

// Windows only: shared by ProcessUtil and the tracing hook, which cannot link
// against the rest of makebelieve.

#include <windows.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace makebelieve {

/// @class TracingEnvironment
/// Provides a namespace for building the environment block a traced process
/// starts with: whatever environment it would have had, with makebelieve's
/// tracing variables - in any case - replaced by this run's.
struct TracingEnvironment {
  /// A variable's name and value.
  using Variable = std::pair<std::wstring_view, std::wstring_view>;

  /// The name of @a entry, a `name=value` string. A name may itself begin
  /// with `=`, as the per-drive current directories do.
  [[nodiscard]] static std::wstring_view name_of(std::wstring_view entry) {
    const std::size_t equals = entry.find(L'=', 1);
    return entry.substr(0, equals);
  }

  /// Whether @a name is one of makebelieve's tracing variables. Windows
  /// matches variable names without regard to case, so this does too.
  [[nodiscard]] static bool is_tracing_variable(std::wstring_view name) {
    constexpr std::wstring_view k_prefix = L"MAKEBELIEVE_TRACE_";
    return name.size() >= k_prefix.size() &&
           CompareStringOrdinal(
               name.data(), static_cast<int>(k_prefix.size()), k_prefix.data(),
               static_cast<int>(k_prefix.size()), TRUE) == CSTR_EQUAL;
  }

  /// A UTF-16 environment block holding @a entries, `name=value` strings,
  /// less any tracing variable in whatever case, plus @a tracing - so the
  /// process sees exactly this run's tracing variables. Sorted by name,
  /// ignoring case, as CreateProcess documents a block must be.
  [[nodiscard]] static std::wstring build(
      std::vector<std::wstring> entries,
      const std::vector<Variable>& tracing) {
    std::erase_if(entries, [](const std::wstring& entry) {
      return is_tracing_variable(name_of(entry));
    });
    for (const auto& [name, value] : tracing) {
      std::wstring entry(name);
      entry.append(L"=").append(value);
      entries.push_back(std::move(entry));
    }
    std::ranges::stable_sort(
        entries, [](const std::wstring& left, const std::wstring& right) {
          const std::wstring_view a = name_of(left);
          const std::wstring_view b = name_of(right);
          return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                      b.data(), static_cast<int>(b.size()),
                                      TRUE) == CSTR_LESS_THAN;
        });
    std::wstring block;
    for (const std::wstring& entry : entries) {
      block.append(entry).push_back(L'\0');
    }
    block.push_back(L'\0');  // the block itself is double-null terminated
    return block;
  }

  /// The entries of this process's own environment.
  [[nodiscard]] static std::vector<std::wstring> own_entries() {
    // Freed however this returns, including by entries_of() throwing.
    const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> env(
        GetEnvironmentStringsW(), &FreeEnvironmentStringsW);
    if (env == nullptr) {
      return {};
    }
    return entries_of(env.get());
  }

  /// The entries of the UTF-16 environment block @a block.
  [[nodiscard]] static std::vector<std::wstring> entries_of(
      const wchar_t* block) {
    std::vector<std::wstring> entries;
    for (const wchar_t* entry = block; *entry != L'\0';) {
      const std::wstring_view view(entry);
      entries.emplace_back(view);
      entry += view.size() + 1;
    }
    return entries;
  }

  /// The entries of the environment block @a block, in the ANSI code page,
  /// as UTF-16.
  [[nodiscard]] static std::vector<std::wstring> entries_of(const char* block) {
    std::vector<std::wstring> entries;
    for (const char* entry = block; *entry != '\0';) {
      const std::string_view view(entry);
      const int length = MultiByteToWideChar(
          CP_ACP, 0, view.data(), static_cast<int>(view.size()), nullptr, 0);
      std::wstring wide(static_cast<std::size_t>(length), L'\0');
      MultiByteToWideChar(CP_ACP, 0, view.data(), static_cast<int>(view.size()),
                          wide.data(), length);
      entries.push_back(std::move(wide));
      entry += view.size() + 1;
    }
    return entries;
  }
};

}  // namespace makebelieve
