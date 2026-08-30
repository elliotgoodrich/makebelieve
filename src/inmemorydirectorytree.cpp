// SPDX-License-Identifier: MIT
#include "inmemorydirectorytree.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace makebelieve {

namespace {

// Maps a tree-relative path onto its normalized, root-relative form (the
// root itself being the empty path), or nullopt when it escapes the root.
// Purely lexical, mirroring RealDirectoryTree: no entry needs to exist for
// this to succeed.
std::optional<std::filesystem::path> normalize(
    const std::filesystem::path& path) {
  if (path.empty()) {
    return std::filesystem::path{};
  }
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return std::nullopt;
  }

  const std::filesystem::path normal = path.lexically_normal();
  const auto first = normal.begin();
  if (first != normal.end() && *first == "..") {
    return std::nullopt;
  }

  // lexically_normal() leaves a path that collapsed to nothing as ".",
  // rather than empty; fold that back onto the root's own sentinel.
  return normal == "." ? std::filesystem::path{} : normal;
}

// Whether @a entry is strictly beneath @a directory (not equal to it).
bool is_within(const std::filesystem::path& entry,
               const std::filesystem::path& directory) {
  const auto result = std::ranges::mismatch(directory, entry);
  return result.in1 == directory.end() && result.in2 != entry.end();
}

}  // namespace

class InMemoryDirectoryTree::Impl {
  struct Node {
    EntryInfo info;
    std::string content;  // Only set when this node is a file.
  };

  std::map<std::filesystem::path, Node> m_entries{
      {std::filesystem::path{}, Node{.info = DirectoryInfo{}}}};

  // Guards m_entries: shared for readers, exclusive for mutators. A mutator
  // releases it before notifying subscribers, so a callback that reads back
  // through the tree takes a fresh shared lock rather than deadlocking.
  mutable std::shared_mutex m_mutex;

  // Guards m_subscribers, held only to add, remove, or copy the callback list,
  // never across a callback. Separate from m_mutex so (un)subscribing does not
  // block the tree's readers.
  mutable std::mutex m_subscribers_mutex;
  mutable std::list<std::function<void(const DirectoryTreeDiff&)>>
      m_subscribers;

 public:
  void write_file(const std::filesystem::path& path,
                  std::string_view content,
                  std::chrono::file_clock::time_point mtime) {
    const std::optional<std::filesystem::path> normalized = normalize(path);
    assert(normalized.has_value() && !normalized->empty() &&
           "write_file() needs a path under the root");

    DirectoryTreeDiff diff;
    {
      const std::unique_lock lock(m_mutex);
      assert(parent_is_directory(*normalized) &&
             "write_file()'s parent directory must already exist");

      const auto it = m_entries.find(*normalized);
      assert((it == m_entries.end() ||
              std::holds_alternative<FileInfo>(it->second.info)) &&
             "write_file() must not target an existing directory");

      diff.entries_changed = {*normalized};
      if (it == m_entries.end()) {
        diff.child_lists_changed = {normalized->parent_path()};
      }
      m_entries[*normalized] =
          Node{.info = FileInfo{.size = content.size(), .mtime = mtime},
               .content = std::string(content)};
    }
    notify_subscribers(diff);
  }

  void make_directory(const std::filesystem::path& path,
                      std::chrono::file_clock::time_point mtime) {
    const std::optional<std::filesystem::path> normalized = normalize(path);
    assert(normalized.has_value() && !normalized->empty() &&
           "make_directory() needs a path under the root");

    DirectoryTreeDiff diff;
    {
      const std::unique_lock lock(m_mutex);
      assert(parent_is_directory(*normalized) &&
             "make_directory()'s parent directory must already exist");
      assert(!m_entries.contains(*normalized) &&
             "make_directory() must not target an existing entry");

      diff.entries_changed = {*normalized};
      diff.child_lists_changed = {normalized->parent_path()};
      m_entries[*normalized] = Node{.info = DirectoryInfo{.mtime = mtime}};
    }
    notify_subscribers(diff);
  }

  void remove(const std::filesystem::path& path) {
    const std::optional<std::filesystem::path> normalized = normalize(path);
    assert(normalized.has_value() && !normalized->empty() &&
           "remove() must not target the root");

    DirectoryTreeDiff diff;
    {
      const std::unique_lock lock(m_mutex);
      assert(m_entries.contains(*normalized) &&
             "remove() must target an existing entry");

      diff.child_lists_changed = {normalized->parent_path()};
      for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->first == *normalized || is_within(it->first, *normalized)) {
          diff.entries_changed.push_back(it->first);
          it = m_entries.erase(it);
        } else {
          ++it;
        }
      }
    }
    notify_subscribers(diff);
  }

  void set_mtime(const std::filesystem::path& path,
                 std::chrono::file_clock::time_point mtime) {
    const std::optional<std::filesystem::path> normalized = normalize(path);
    assert(normalized.has_value() && "set_mtime() must not escape the root");

    DirectoryTreeDiff diff;
    {
      const std::unique_lock lock(m_mutex);
      const auto it = m_entries.find(*normalized);
      assert(it != m_entries.end() &&
             "set_mtime() must target an existing entry");
      std::visit([mtime](auto& info) { info.mtime = mtime; }, it->second.info);

      diff.entries_changed = {*normalized};
    }
    notify_subscribers(diff);
  }

  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const {
    const std::optional<std::filesystem::path> normalized = normalize(path);
    if (!normalized.has_value()) {
      return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }

    const std::shared_lock lock(m_mutex);
    const auto it = m_entries.find(*normalized);
    if (it == m_entries.end()) {
      return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    return it->second.info;
  }

  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path) const {
    const std::optional<std::filesystem::path> normalized = normalize(path);
    if (!normalized.has_value()) {
      return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }

    const std::shared_lock lock(m_mutex);
    const auto it = m_entries.find(*normalized);
    if (it == m_entries.end() ||
        !std::holds_alternative<DirectoryInfo>(it->second.info)) {
      return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }

    // entry_path != *normalized excludes the root from its own listing: ""
    // is its own parent_path(), which every other entry's key is exempt
    // from (a child's key is always strictly longer than its parent's).
    std::vector<TreeEntry> entries;
    for (const auto& [entry_path, node] : m_entries) {
      if (entry_path != *normalized &&
          entry_path.parent_path() == *normalized) {
        entries.push_back({.name = entry_path.filename(), .info = node.info});
      }
    }
    return entries;
  }

  // offset and size are the signature DirectoryTree::read() defines, so they
  // cannot be reordered or renamed apart here.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size) const {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const std::optional<std::filesystem::path> normalized = normalize(path);
    if (!normalized.has_value()) {
      return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (offset < 0) {
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if (size == 0) {
      return std::string{};
    }

    const std::shared_lock lock(m_mutex);
    const auto it = m_entries.find(*normalized);
    if (it == m_entries.end()) {
      return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (!std::holds_alternative<FileInfo>(it->second.info)) {
      return std::unexpected(std::make_error_code(std::errc::is_a_directory));
    }

    const std::string& content = it->second.content;
    if (static_cast<std::size_t>(offset) >= content.size()) {
      return std::string{};
    }
    return content.substr(static_cast<std::size_t>(offset), size);
  }

  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) const {
    const std::lock_guard lock(m_subscribers_mutex);
    const auto it = m_subscribers.emplace(m_subscribers.end(), callback);
    return Subscription([this, it] {
      const std::lock_guard lock(m_subscribers_mutex);
      m_subscribers.erase(it);
    });
  }

 private:
  [[nodiscard]] bool parent_is_directory(
      const std::filesystem::path& normalized) const {
    const auto it = m_entries.find(normalized.parent_path());
    return it != m_entries.end() &&
           std::holds_alternative<DirectoryInfo>(it->second.info);
  }

  // Invokes every registered callback with @a diff, from a copy of the list
  // taken under m_subscribers_mutex so a callback may (un)subscribe while it
  // runs. Must be called with m_mutex unlocked, so a callback can read the
  // tree.
  void notify_subscribers(const DirectoryTreeDiff& diff) const {
    std::vector<std::function<void(const DirectoryTreeDiff&)>> callbacks;
    {
      const std::lock_guard lock(m_subscribers_mutex);
      callbacks.assign(m_subscribers.cbegin(), m_subscribers.cend());
    }
    for (const std::function<void(const DirectoryTreeDiff&)>& callback :
         callbacks) {
      callback(diff);
    }
  }
};

InMemoryDirectoryTree::InMemoryDirectoryTree()
    : m_impl(std::make_unique<Impl>()) {}

InMemoryDirectoryTree::~InMemoryDirectoryTree() = default;

void InMemoryDirectoryTree::write_file(
    const std::filesystem::path& path,
    std::string_view content,
    std::chrono::file_clock::time_point mtime) {
  m_impl->write_file(path, content, mtime);
}

void InMemoryDirectoryTree::make_directory(
    const std::filesystem::path& path,
    std::chrono::file_clock::time_point mtime) {
  m_impl->make_directory(path, mtime);
}

void InMemoryDirectoryTree::remove(const std::filesystem::path& path) {
  m_impl->remove(path);
}

void InMemoryDirectoryTree::set_mtime(
    const std::filesystem::path& path,
    std::chrono::file_clock::time_point mtime) {
  m_impl->set_mtime(path, mtime);
}

std::expected<EntryInfo, std::error_code> InMemoryDirectoryTree::status(
    const std::filesystem::path& path) const {
  return m_impl->status(path);
}

std::expected<std::vector<TreeEntry>, std::error_code>
InMemoryDirectoryTree::ls(const std::filesystem::path& path) const {
  return m_impl->ls(path);
}

std::expected<std::string, std::error_code> InMemoryDirectoryTree::read(
    const std::filesystem::path& path,
    Offset offset,
    std::size_t size) const {
  return m_impl->read(path, offset, size);
}

Subscription InMemoryDirectoryTree::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  return m_impl->subscribe_to_changes(callback);
}

}  // namespace makebelieve
