// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <compare>
#include <concepts>
#include <filesystem>
#include <type_traits>
#include <utility>
#include <vector>

namespace makebelieve {

/// \struct Input
/// A path a command looked at, and so depends on: a file it read or got the
/// attributes of, on the file's content; a directory it got the attributes
/// of, on there being a directory there; a directory it listed, on its
/// entries too; and a path it found nothing at, on there being nothing there.
struct Input {
  std::filesystem::path path;

  /// Whether it listed the directory at `path`, and so depends on its
  /// entries, not just on there being a directory there.
  bool listed = false;

  Input() = default;

  /// An input at @a path, listed if @a listed. Implicit, so a path, or
  /// anything a path is made from, stands for one it only looked at.
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  Input(std::filesystem::path path, bool listed = false)
      : path(std::move(path)), listed(listed) {}

  /// As above, from whatever a path is made from, such as a string literal.
  template <typename Source>
    requires(!std::same_as<std::remove_cvref_t<Source>,
                           std::filesystem::path> &&
             std::constructible_from<std::filesystem::path, const Source&>)
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  Input(const Source& path, bool listed = false) : path(path), listed(listed) {}

  [[nodiscard]] friend bool operator==(const Input&, const Input&) = default;
  [[nodiscard]] friend auto operator<=>(const Input&, const Input&) = default;
};

/// Sorts @a inputs by path and leaves each path once, listed if any of its
/// entries was.
inline void merge_inputs(std::vector<Input>& inputs) {
  std::ranges::sort(inputs, {}, &Input::path);
  auto out = inputs.begin();
  for (auto it = inputs.begin(); it != inputs.end();) {
    Input merged = std::move(*it);
    for (++it; it != inputs.end() && it->path == merged.path; ++it) {
      merged.listed = merged.listed || it->listed;
    }
    *out++ = std::move(merged);
  }
  inputs.erase(out, inputs.end());
}

}  // namespace makebelieve
