// SPDX-License-Identifier: MIT
#include "directorytreeutil.hpp"

#include "directorytree.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace makebelieve {

namespace {

std::string describe(const std::filesystem::path& path) {
  return path.empty() ? std::string("<root>") : path.generic_string();
}

std::string kind_name(const EntryInfo& info) {
  return std::holds_alternative<FileInfo>(info) ? "a file" : "a directory";
}

std::string join(const std::vector<std::string>& values) {
  std::string result;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      result += ", ";
    }
    result += values[i];
  }
  return result;
}

std::vector<std::string> sorted_names(const std::vector<TreeEntry>& entries) {
  std::vector<std::string> names;
  names.reserve(entries.size());
  for (const TreeEntry& entry : entries) {
    names.push_back(entry.name.string());
  }
  std::ranges::sort(names);
  return names;
}

// Recurses one level per directory nesting, so depth is bounded by the
// tree's own depth rather than anything attacker- or caller-controlled.
// NOLINTNEXTLINE(misc-no-recursion)
std::string diff_at(const DirectoryTree& a,
                    const DirectoryTree& b,
                    const std::filesystem::path& path) {
  const std::expected<EntryInfo, std::error_code> status_a = a.status(path);
  const std::expected<EntryInfo, std::error_code> status_b = b.status(path);

  if (status_a.has_value() != status_b.has_value()) {
    return std::format("{}: exists in {} but not the other", describe(path),
                       status_a.has_value() ? "a" : "b");
  }
  if (!status_a.has_value()) {
    return {};  // Missing from both - nothing further to compare here.
  }

  if (status_a->index() != status_b->index()) {
    return std::format("{}: is {} in a and {} in b", describe(path),
                       kind_name(*status_a), kind_name(*status_b));
  }

  if (const auto* file_a = std::get_if<FileInfo>(&*status_a)) {
    const auto& file_b = std::get<FileInfo>(*status_b);
    const std::expected<std::string, std::error_code> content_a =
        a.read(path, 0, file_a->size);
    const std::expected<std::string, std::error_code> content_b =
        b.read(path, 0, file_b.size);
    if (!content_a.has_value() || !content_b.has_value()) {
      return std::format("{}: failed to read from {}", describe(path),
                         !content_a.has_value() ? "a" : "b");
    }
    if (*content_a != *content_b) {
      return std::format("{}: content differs ({} bytes in a, {} bytes in b)",
                         describe(path), content_a->size(), content_b->size());
    }
    return {};
  }

  const std::expected<std::vector<TreeEntry>, std::error_code> ls_a =
      a.ls(path);
  const std::expected<std::vector<TreeEntry>, std::error_code> ls_b =
      b.ls(path);
  if (!ls_a.has_value() || !ls_b.has_value()) {
    return std::format("{}: failed to list {}", describe(path),
                       !ls_a.has_value() ? "a" : "b");
  }

  const std::vector<std::string> names_a = sorted_names(*ls_a);
  const std::vector<std::string> names_b = sorted_names(*ls_b);
  if (names_a != names_b) {
    return std::format("{}: children differ (a has [{}], b has [{}])",
                       describe(path), join(names_a), join(names_b));
  }

  for (const std::string& name : names_a) {
    std::string child_diff = diff_at(a, b, path / name);
    if (!child_diff.empty()) {
      return child_diff;
    }
  }
  return {};
}

}  // namespace

std::string DirectoryTreeUtil::diff(const DirectoryTree& a,
                                    const DirectoryTree& b) {
  return diff_at(a, b, std::filesystem::path{});
}

bool DirectoryTreeUtil::equal(const DirectoryTree& a, const DirectoryTree& b) {
  return diff(a, b).empty();
}

}  // namespace makebelieve
