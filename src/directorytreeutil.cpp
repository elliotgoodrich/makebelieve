// SPDX-License-Identifier: MIT
#include "directorytreeutil.hpp"

#include "directorytree.hpp"

#include <exec/task.hpp>

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

  if (std::holds_alternative<FileInfo>(*status_a)) {
    const std::expected<std::string, std::error_code> content_a =
        DirectoryTreeUtil::wait_read(DirectoryTreeUtil::read_whole(a, path));
    const std::expected<std::string, std::error_code> content_b =
        DirectoryTreeUtil::wait_read(DirectoryTreeUtil::read_whole(b, path));
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

exec::task<std::expected<std::string, std::error_code>> read_file_range(
    const DirectoryTree& tree,
    std::filesystem::path path,
    Offset offset,
    std::size_t size) {
  // Keep the path-read convention for empty requests and invalid offsets,
  // including rejection of an escaping path before examining the range.
  if (size == 0 || offset < 0) {
    const std::filesystem::path normal = path.lexically_normal();
    if (path.has_root_path() ||
        (normal.begin() != normal.end() && *normal.begin() == "..")) {
      co_return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (offset < 0) {
      co_return std::unexpected(
          std::make_error_code(std::errc::invalid_argument));
    }
    co_return std::string{};
  }
  std::expected<std::shared_ptr<const OpenedFile>, std::error_code> opened =
      co_await tree.open(path);
  if (!opened) {
    co_return std::unexpected(opened.error());
  }
  co_return co_await (*opened)->read(offset, size);
}

exec::task<std::expected<std::string, std::error_code>> read_whole_file(
    const DirectoryTree& tree,
    std::filesystem::path path,
    OpenContext context) {
  std::expected<std::shared_ptr<const OpenedFile>, std::error_code> opened =
      co_await tree.open(path, context);
  if (!opened) {
    co_return std::unexpected(opened.error());
  }
  constexpr std::size_t chunk_size = std::size_t{64} * 1024;
  std::string content;
  for (;;) {
    std::expected<std::string, std::error_code> chunk =
        co_await (*opened)->read(static_cast<Offset>(content.size()),
                                 chunk_size);
    if (!chunk) {
      co_return std::unexpected(chunk.error());
    }
    content += *chunk;
    if (chunk->size() < chunk_size) {
      co_return content;
    }
  }
}

}  // namespace

std::expected<std::shared_ptr<const OpenedFile>, std::error_code>
DirectoryTreeUtil::open(const DirectoryTree& tree,
                        const std::filesystem::path& path,
                        const OpenContext& context) {
  auto result = stdexec::sync_wait(stdexec::write_env(
      tree.open(path, context),
      stdexec::prop{stdexec::get_stop_token, stdexec::inplace_stop_token{}}));
  if (!result) {
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));
  }
  return std::move(std::get<0>(*result));
}

std::expected<std::string, std::error_code> DirectoryTreeUtil::read(
    const DirectoryTree& tree,
    const std::filesystem::path& path,
    Offset offset,
    std::size_t size) {
  return wait_read(read_range(tree, path, offset, size));
}

ReadSender DirectoryTreeUtil::read_range(const DirectoryTree& tree,
                                         const std::filesystem::path& path,
                                         Offset offset,
                                         std::size_t size) {
  return stdexec::write_env(
      read_file_range(tree, path, offset, size),
      stdexec::prop{stdexec::get_start_scheduler, stdexec::inline_scheduler{}});
}

ReadSender DirectoryTreeUtil::read_whole(const DirectoryTree& tree,
                                         const std::filesystem::path& path,
                                         const OpenContext& context) {
  return stdexec::write_env(
      read_whole_file(tree, path, context),
      stdexec::prop{stdexec::get_start_scheduler, stdexec::inline_scheduler{}});
}

std::expected<std::string, std::error_code> DirectoryTreeUtil::wait_read(
    ReadSender sender) {
  auto result = stdexec::sync_wait(stdexec::write_env(
      std::move(sender),
      stdexec::prop{stdexec::get_stop_token, stdexec::inplace_stop_token{}}));
  if (!result) {
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));
  }
  return std::move(std::get<0>(*result));
}

std::string DirectoryTreeUtil::diff(const DirectoryTree& a,
                                    const DirectoryTree& b) {
  return diff_at(a, b, std::filesystem::path{});
}

bool DirectoryTreeUtil::equal(const DirectoryTree& a, const DirectoryTree& b) {
  return diff(a, b).empty();
}

}  // namespace makebelieve
