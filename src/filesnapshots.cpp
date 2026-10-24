// SPDX-License-Identifier: MIT
#include "filesnapshots.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace makebelieve {

namespace {

bool same(const FileInfo& a, const FileInfo& b) {
  return a.size == b.size && a.mtime == b.mtime;
}

std::expected<FileInfo, std::error_code> file_status(
    const DirectoryTree& tree,
    const std::filesystem::path& path) {
  const std::expected<EntryInfo, std::error_code> status = tree.status(path);
  if (!status.has_value()) {
    return std::unexpected(status.error());
  }
  if (const auto* file = std::get_if<FileInfo>(&*status)) {
    return *file;
  }
  return std::unexpected(std::make_error_code(std::errc::is_a_directory));
}

}  // namespace

struct FileSnapshots::Current {
  struct Entry {
    std::weak_ptr<const FileSnapshot> snapshot;
    // Which snapshot this is, to tell it apart once `snapshot` has expired.
    // Only ever compared, never followed.
    const FileSnapshot* address;
  };

  // Never held while a snapshot is released, since releasing the last
  // reference to one takes it (see Forget).
  std::mutex mutex;
  std::unordered_map<std::filesystem::path, Entry> snapshots;
};

// Deletes a snapshot, first taking it out of Current if it is still the one
// there - a newer snapshot of the file may have taken its place.
class FileSnapshots::Forget {
  std::weak_ptr<Current> m_current;

 public:
  explicit Forget(std::weak_ptr<Current> current)
      : m_current(std::move(current)) {}

  void operator()(FileSnapshot* snapshot) const noexcept {
    if (const std::shared_ptr<Current> current = m_current.lock()) {
      const std::lock_guard lock(current->mutex);
      if (const auto it = current->snapshots.find(snapshot->path);
          it != current->snapshots.end() && it->second.address == snapshot) {
        current->snapshots.erase(it);
      }
    }
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): this is its deleter.
    delete snapshot;
  }
};

FileSnapshots::FileSnapshots(const DirectoryTree& tree)
    : m_tree(tree), m_current(std::make_shared<Current>()) {}

void FileSnapshots::changed(const DirectoryTreeDiff& diff) {
  const std::lock_guard lock(m_mutex);
  const std::lock_guard current_lock(m_current->mutex);
  if (diff.everything_dirty) {
    m_everything = ++m_clock;
    m_generations.clear();
    m_current->snapshots.clear();
    return;
  }
  for (const std::filesystem::path& path : diff.entries_changed) {
    m_generations.insert_or_assign(path, ++m_clock);
    m_current->snapshots.erase(path);
  }
}

std::uint64_t FileSnapshots::generation(
    const std::filesystem::path& path) const {
  const std::lock_guard lock(m_mutex);
  const auto it = m_generations.find(path);
  return std::max(m_everything, it != m_generations.end() ? it->second : 0);
}

std::uint64_t FileSnapshots::latest() const {
  const std::lock_guard lock(m_mutex);
  return m_clock;
}

std::size_t FileSnapshots::held() const {
  const std::lock_guard lock(m_current->mutex);
  return m_current->snapshots.size();
}

exec::task<std::expected<std::shared_ptr<const FileSnapshot>, std::error_code>>
FileSnapshots::take(std::filesystem::path path) const {
  const std::uint64_t before = generation(path);
  const std::expected<FileInfo, std::error_code> info =
      file_status(m_tree, path);
  if (!info.has_value()) {
    co_return std::unexpected(info.error());
  }
  // Taken out under the lock but looked at outside it: if it is the last
  // reference, letting it go takes the lock (see Forget).
  std::shared_ptr<const FileSnapshot> held;
  {
    const std::lock_guard lock(m_current->mutex);
    if (const auto it = m_current->snapshots.find(path);
        it != m_current->snapshots.end()) {
      held = it->second.snapshot.lock();
    }
  }
  if (held && held->generation == before && same(held->info(), *info)) {
    co_return held;
  }
  held.reset();

  std::expected<FileContent, std::error_code> content =
      co_await m_tree.read_whole(path);
  if (!content.has_value()) {
    co_return std::unexpected(content.error());
  }
  const std::shared_ptr<const FileSnapshot> snapshot(
      new FileSnapshot{.path = path,
                       .bytes = std::move(content->bytes),
                       .mtime = content->info.mtime,
                       .generation = before},
      Forget(m_current));
  {
    const std::lock_guard lock(m_current->mutex);
    m_current->snapshots.insert_or_assign(
        path, Current::Entry{.snapshot = snapshot, .address = snapshot.get()});
  }
  co_return snapshot;
}

}  // namespace makebelieve
