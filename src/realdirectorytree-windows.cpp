// SPDX-License-Identifier: MIT
#include "realdirectorytree.hpp"

#include "directorytreeutil.hpp"
#include "iocontext.hpp"
#include "scopeguard.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

namespace makebelieve {

namespace {

// Everything under the root that could change a build input. Attribute and
// creation changes are included because a rule may key off them, and the
// notification is cheap next to the rebuild it might avoid.
constexpr DWORD k_watch_filter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;

constexpr DWORD k_watch_buffer_bytes = 64 * 1024;

// ReadFile takes a DWORD count, so a request larger than 4GiB has to be read
// across several calls. Capping well below the DWORD limit also keeps a
// single call from pinning an unreasonable span of the output buffer.
constexpr std::uint64_t k_read_chunk_bytes = 32ULL * 1024 * 1024;

[[noreturn]] void throw_last_error(const char* what) {
  throw std::system_error(static_cast<int>(GetLastError()),
                          std::system_category(), what);
}

// Returns the calling thread's last Win32 error as a std::error_code.
std::error_code last_error_code() {
  return std::error_code(static_cast<int>(GetLastError()),
                         std::system_category());
}

// Splices a Win32 high/low pair into one 64-bit value.
std::uint64_t combine(DWORD high, DWORD low) {
  return (static_cast<std::uint64_t>(high) << 32) | low;
}

// Converts a FILETIME into a `file_clock::time_point`.
std::chrono::file_clock::time_point to_file_time(const FILETIME& time) {
  // MSVC's file_clock counts 100ns ticks from the Windows epoch (1601-01-01),
  // which is exactly FILETIME's representation, so this is a reinterpretation
  // rather than a conversion.
  return std::chrono::file_clock::time_point(
      std::chrono::file_clock::duration(static_cast<std::int64_t>(
          combine(time.dwHighDateTime, time.dwLowDateTime))));
}

// How many times read_whole() reads a file that changes under it before
// handing out what it last read.
constexpr int k_read_whole_attempts = 8;

bool same_version(const BY_HANDLE_FILE_INFORMATION& a,
                  const BY_HANDLE_FILE_INFORMATION& b) {
  return a.nFileSizeHigh == b.nFileSizeHigh &&
         a.nFileSizeLow == b.nFileSizeLow &&
         a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
         a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}

// Builds an EntryInfo from the fields both WIN32_FIND_DATAW and
// WIN32_FILE_ATTRIBUTE_DATA carry, so ls() and status() cannot drift apart.
EntryInfo make_info(DWORD attributes,
                    DWORD size_high,
                    DWORD size_low,
                    const FILETIME& last_write) {
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return DirectoryInfo{to_file_time(last_write)};
  }
  // FileInfo::size is std::size_t, so this truncates for a file above 4GiB
  // on a 32-bit build.
  return FileInfo{static_cast<std::size_t>(combine(size_high, size_low)),
                  to_file_time(last_write)};
}

// Walks the FILE_NOTIFY_INFORMATION chain into a diff.
//
// Three details of that structure are each easy to get wrong: FileName is
// relative to the watch root, FileNameLength counts bytes rather than
// characters, and the name is not null-terminated.
//
// Every record names something whose own identity moved, so each one lands
// in entries_changed regardless of action. Only appearing and disappearing
// changes a *parent's* child list, so those four actions additionally mark
// the parent. A plain write does not: the name set is untouched.
//
// Because both lists are sets, the action no longer needs classifying and
// repeated records for one path collapse on their own - a file created and
// then written in the same batch is one entry, and a rename is simply two
// paths that both moved.
//
// Directories land in entries_changed alongside files. The notification
// carries no way to tell them apart, and stat-ing each one to find out
// would cost a round trip per change.
DirectoryTreeDiff parse_changes(const std::byte* buffer,
                                DWORD bytes,
                                const std::filesystem::path& root,
                                const BuildPaths& excluded,
                                const std::filesystem::path& relative,
                                std::set<std::filesystem::path>* renamed) {
  // Ordered sets: one directory gaining a thousand entries has to collapse
  // to a single child_lists_changed entry, and sorted output is what
  // makes each list cheap to merge against state a subscriber already holds.
  std::set<std::filesystem::path> files;
  std::set<std::filesystem::path> directories;

  for (DWORD offset = 0; offset < bytes;) {
    // Each record is DWORD-aligned by the kernel and the buffer itself came
    // from the allocator, so this is suitably aligned to read through.
    const auto* record =
        reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    const std::wstring_view name(record->FileName,
                                 record->FileNameLength / sizeof(WCHAR));
    const auto path = relative / name;
    if (excluded.contains(root / path)) {
      if (record->NextEntryOffset == 0) {
        break;
      }
      offset += record->NextEntryOffset;
      continue;
    }
    const auto it = files.emplace(path).first;

    switch (record->Action) {
      case FILE_ACTION_ADDED:
      case FILE_ACTION_REMOVED:
      case FILE_ACTION_RENAMED_OLD_NAME:
      case FILE_ACTION_RENAMED_NEW_NAME:
        if (renamed) {
          renamed->insert(path);
        }
        // parent_path() of a root-level entry is the empty path, which is
        // exactly how the interface spells the tree root.
        directories.emplace(it->parent_path());
        break;
      case FILE_ACTION_MODIFIED:
      default:
        // Anything unrecognised has already been recorded in entries_changed,
        // which is the conservative half. Leaving the parent alone risks
        // missing a listing change, so if an unknown action ever shows up
        // that is the direction to widen.
        break;
    }

    if (record->NextEntryOffset == 0) {
      break;
    }
    offset += record->NextEntryOffset;
  }

  DirectoryTreeDiff diff;
  diff.entries_changed.assign(std::make_move_iterator(files.begin()),
                              std::make_move_iterator(files.end()));
  diff.child_lists_changed.assign(std::make_move_iterator(directories.begin()),
                                  std::make_move_iterator(directories.end()));
  return diff;
}

// ReadDirectoryChangesW cannot exclude a subtree. Each ancestor of an
// exclusion gets a shallow watch. Its other children each get one recursive
// watch, so a disjoint build directory still needs just one watch in total.
struct DirectoryWatch {
  std::filesystem::path relative;
  bool recursive = false;
  HANDLE directory = INVALID_HANDLE_VALUE;
  HANDLE event = nullptr;
  OVERLAPPED overlapped{};
  std::vector<std::byte> buffer;
  bool pending = false;
  stdexec::counting_scope waiting;

  ~DirectoryWatch() {
    if (pending) {
      DWORD bytes;
      CancelIoEx(directory, &overlapped);
      GetOverlappedResult(directory, &overlapped, &bytes, TRUE);
    }
    if (directory != INVALID_HANDLE_VALUE) {
      CloseHandle(directory);
    }
    if (event) {
      CloseHandle(event);
    }
  }

  bool issue_read() {
    ResetEvent(event);
    overlapped = {};
    overlapped.hEvent = event;
    pending = ReadDirectoryChangesW(directory, buffer.data(),
                                    static_cast<DWORD>(buffer.size()),
                                    recursive, k_watch_filter, nullptr,
                                    &overlapped, nullptr) != 0;
    return pending;
  }
};

}  // namespace

class RealDirectoryTree::Impl {
  std::filesystem::path m_root;
  BuildPaths m_excluded;
  IoContext& m_io;

  // Everything below is mutable because the whole interface is const: these
  // are the internals that const-as-thread-safety permits us to touch, and
  // each is guarded either by m_mutex or by being the watch's alone.

  // Guards m_subscribers, m_next_id, and the lazy watcher startup.
  mutable std::mutex m_mutex;
  mutable std::list<std::function<void(const DirectoryTreeDiff&)>>
      m_subscribers;

  // Held for a whole round of callbacks, and by unsubscribing, so that an
  // unsubscribe returns only once none of its callbacks can still be running.
  // Recursive so that a callback may still (un)subscribe from within itself.
  // Taken before m_mutex.
  mutable std::recursive_mutex m_notifying;

  // Changed only on the IoContext thread. A removed directory's event and
  // buffer outlive its last callback, even if its name is immediately reused.
  mutable std::map<std::filesystem::path, std::shared_ptr<DirectoryWatch>>
      m_watches;
  mutable stdexec::counting_scope m_retiring;
  mutable bool m_watching_started = false;

 public:
  Impl(const std::filesystem::path& root, IoContext& io, BuildPaths excluded)
      : m_root(std::filesystem::weakly_canonical(root)),
        m_excluded(std::move(excluded)),
        m_io(io) {}

  ~Impl() {
    if (m_watching_started) {
      stdexec::sync_wait(stdexec::schedule(m_io.get_scheduler()) |
                         stdexec::then([this] { retire_watches({}); }));
      stdexec::sync_wait(m_retiring.join());
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const;
  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path) const;
  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size) const;
  [[nodiscard]] std::expected<FileContent, std::error_code> read_whole(
      const std::filesystem::path& path) const;
  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) const;

 private:
  // Maps a tree-relative path onto an absolute one, or nullopt when it
  // escapes the root.
  [[nodiscard]] std::optional<std::filesystem::path> resolve(
      const std::filesystem::path& path) const;

  // Opens the root for change notifications, issues the first read, and starts
  // waiting on it. Called once, from the first subscribe_to_changes(), with
  // m_mutex held.
  void start_watcher() const;

  void add_watch(const std::filesystem::path& relative) const;
  void watch_children(const DirectoryWatch& parent) const;
  void retire_watches(const std::filesystem::path& relative) const;
  void on_change(DirectoryWatch& watch) const;

  // Invokes every registered callback with `diff`. Takes a copy of the
  // callback list so that a callback which unsubscribes cannot deadlock
  // against m_mutex.
  void notify_subscribers(const DirectoryTreeDiff& diff) const;
};

std::optional<std::filesystem::path> RealDirectoryTree::Impl::resolve(
    const std::filesystem::path& path) const {
  if (path.empty()) {
    return m_root;
  }

  // An absolute or drive-qualified path would make operator/ discard the root
  // outright, so both are refused before anything else looks at them.
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return std::nullopt;
  }

  // Normalizing first collapses interior "..", so a leading ".." afterwards
  // is exactly the set of paths that climb out of the root.
  const std::filesystem::path normal = path.lexically_normal();
  const auto first = normal.begin();
  if (first != normal.end() && *first == "..") {
    return std::nullopt;
  }

  const auto resolved = m_root / normal;
  if (m_excluded.contains_resolved(resolved)) {
    return std::nullopt;
  }
  return resolved;
}

std::expected<EntryInfo, std::error_code> RealDirectoryTree::Impl::status(
    const std::filesystem::path& path) const {
  // An escape reads as "no such entry"; an on-disk failure is reported as the
  // OS gave it, so a mount can tell a reader "access denied" or "path not
  // found" rather than claiming the entry itself is missing.
  const std::optional<std::filesystem::path> absolute = resolve(path);
  if (!absolute.has_value()) {
    return std::unexpected(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(absolute->c_str(), GetFileExInfoStandard, &data)) {
    return std::unexpected(last_error_code());
  }
  return make_info(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow,
                   data.ftLastWriteTime);
}

std::expected<std::vector<TreeEntry>, std::error_code>
RealDirectoryTree::Impl::ls(const std::filesystem::path& path) const {
  // An escape reads as "no such entry"; a failure to list the directory is
  // reported as the OS gave it. A present-but-empty vector (an empty
  // directory) is a success.
  const std::optional<std::filesystem::path> resolved = resolve(path);
  if (!resolved.has_value()) {
    return std::unexpected(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  // FindExInfoBasic skips the 8.3 short name, which we never use, and
  // LARGE_FETCH batches directory reads. Both matter for the wide directories a
  // build tree tends to have.
  WIN32_FIND_DATAW data{};
  const std::filesystem::path pattern = *resolved / L"*";
  const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) {
    // Only a volume's root has no "." to find, and so reports an empty listing
    // this way.
    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
      return std::vector<TreeEntry>{};
    }
    return std::unexpected(last_error_code());
  }
  const ScopeGuard close_find([find] { FindClose(find); });

  std::vector<TreeEntry> entries;
  do {
    const std::wstring_view name = data.cFileName;
    if (name == L"." || name == L".." ||
        m_excluded.contains(*resolved / name) ||
        ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
         m_excluded.contains_resolved(*resolved / name))) {
      continue;
    }
    entries.emplace_back(std::filesystem::path(name),
                         make_info(data.dwFileAttributes, data.nFileSizeHigh,
                                   data.nFileSizeLow, data.ftLastWriteTime));
  } while (FindNextFileW(find, &data));
  // FindNextFileW reports the end and a failure the same way.
  if (GetLastError() != ERROR_NO_MORE_FILES) {
    return std::unexpected(last_error_code());
  }
  return entries;
}

std::expected<std::string, std::error_code> RealDirectoryTree::Impl::read(
    const std::filesystem::path& path,
    Offset offset,
    std::size_t size) const {
  const std::optional<std::filesystem::path> resolved = resolve(path);
  if (!resolved.has_value()) {
    return std::unexpected(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  if (offset < 0) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }
  if (size == 0) {
    return std::string{};
  }

  // Sharing every mode matters here: a build tool may well be holding the
  // file open, and refusing to read because of that would be worse than
  // reading a version that is about to change - the watcher will tell us.
  //
  // Opening per call is the obvious thing to improve once this is hot. ProjFS
  // hydrates a large file across many calls, and each one currently pays for
  // an open and a close; a small handle cache keyed on the resolved path is
  // the natural fix, and it needs to be one, given every method here is
  // callable concurrently.
  const HANDLE file =
      CreateFileW(resolved->c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::unexpected(last_error_code());
  }
  const ScopeGuard close_file([file] { CloseHandle(file); });

  std::string contents;
  std::optional<std::error_code> failure;
  contents.resize_and_overwrite(size, [&](char* const data,
                                          const std::size_t capacity) noexcept {
    std::size_t total = 0;
    while (total < capacity) {
      // The handle is synchronous, so passing an OVERLAPPED here does not
      // make the read asynchronous - it just supplies the file position,
      // which is what lets concurrent reads of the same path share no
      // state.
      const std::uint64_t position = static_cast<std::uint64_t>(offset) + total;
      OVERLAPPED overlapped{};
      overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
      overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);

      const DWORD chunk = static_cast<DWORD>(
          std::min<std::uint64_t>(capacity - total, k_read_chunk_bytes));
      DWORD read_bytes = 0;
      if (!ReadFile(file, data + total, chunk, &read_bytes, &overlapped)) {
        // A positioned read that starts at or past the end reports itself
        // this way rather than as a zero-byte success.
        if (GetLastError() != ERROR_HANDLE_EOF) {
          failure = last_error_code();
        }
        break;
      }
      if (read_bytes == 0) {
        break;
      }
      total += read_bytes;
    }
    return total;
  });

  if (failure.has_value()) {
    return std::unexpected(*failure);
  }
  // Short of what was asked for means end of file, which is the contract the
  // base class states. The ProjFS adapter is what has to turn that into a
  // full-range answer.
  return contents;
}

Subscription RealDirectoryTree::Impl::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  const std::lock_guard<std::mutex> lock(m_mutex);

  if (!m_watching_started) {
    start_watcher();
  }

  const auto it = m_subscribers.emplace(m_subscribers.end(), callback);
  return Subscription([this, it] {
    const std::lock_guard notifying(m_notifying);
    const std::lock_guard<std::mutex> unsubscribe_lock(m_mutex);
    m_subscribers.erase(it);
  });
}

void RealDirectoryTree::Impl::start_watcher() const {
  m_watching_started = true;
  stdexec::sync_wait(stdexec::schedule(m_io.get_scheduler()) |
                     stdexec::then([this] { add_watch({}); }));
}

void RealDirectoryTree::Impl::add_watch(
    const std::filesystem::path& relative) const {
  const auto path = relative.empty() ? m_root : m_root / relative;
  if (m_excluded.contains(path) || m_watches.contains(relative)) {
    return;
  }
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (!relative.empty() && (attributes == INVALID_FILE_ATTRIBUTES ||
                            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
    return;
  }
  auto watch = std::make_shared<DirectoryWatch>();
  watch->relative = relative;
  watch->recursive = !BuildPaths::within(m_excluded.mount, path) &&
                     !BuildPaths::within(m_excluded.cache, path) &&
                     !BuildPaths::within(m_excluded.scratch, path);
  watch->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!watch->event) {
    throw_last_error("CreateEventW");
  }
  watch->directory =
      CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (watch->directory == INVALID_HANDLE_VALUE) {
    if (!relative.empty() && (GetLastError() == ERROR_FILE_NOT_FOUND ||
                              GetLastError() == ERROR_PATH_NOT_FOUND)) {
      return;
    }
    throw_last_error("CreateFileW (watch directory)");
  }
  watch->buffer.resize(k_watch_buffer_bytes);
  if (!watch->issue_read()) {
    throw_last_error("ReadDirectoryChangesW");
  }
  m_watches.emplace(relative, watch);
  m_io.spawn_watch(
      watch->event, watch->waiting,
      [this, watch]() noexcept {
        try {
          on_change(*watch);
        } catch (const std::exception& error) {
          m_io.report_watch_failure();
          std::println(stderr, "makebelieve: watching the source directory: {}",
                       error.what());
        }
      },
      [](std::error_code error) noexcept {
        std::println(stderr,
                     "makebelieve: stopped watching the source directory: {}",
                     error.message());
      });
  if (!watch->recursive) {
    watch_children(*watch);
  }
}

void RealDirectoryTree::Impl::watch_children(
    const DirectoryWatch& parent) const {
  for (const auto& child :
       std::filesystem::directory_iterator(m_root / parent.relative)) {
    add_watch(parent.relative / child.path().filename());
  }
}

void RealDirectoryTree::Impl::retire_watches(
    const std::filesystem::path& relative) const {
  for (auto it = m_watches.begin(); it != m_watches.end();) {
    if (!relative.empty() && !BuildPaths::within(it->first, relative)) {
      ++it;
      continue;
    }
    auto watch = it->second;
    it = m_watches.erase(it);
    watch->waiting.request_stop();
    stdexec::spawn(
        stdexec::write_env(
            watch->waiting.join(),
            stdexec::prop{stdexec::get_start_scheduler, m_io.get_scheduler()}) |
            stdexec::then([watch]() noexcept {}),
        m_retiring.get_token());
  }
}

void RealDirectoryTree::Impl::on_change(DirectoryWatch& watch) const {
  DWORD bytes = 0;
  watch.pending = false;
  if (!GetOverlappedResult(watch.directory, &watch.overlapped, &bytes, FALSE)) {
    ResetEvent(watch.event);
    return;
  }
  std::set<std::filesystem::path> renamed;
  DirectoryTreeDiff diff =
      bytes == 0
          ? DirectoryTreeDiff{.everything_dirty = true}
          : parse_changes(watch.buffer.data(), bytes, m_root, m_excluded,
                          watch.relative, watch.recursive ? nullptr : &renamed);
  if (!watch.issue_read()) {
    throw_last_error("ReadDirectoryChangesW");
  }
  if (!watch.recursive && diff.everything_dirty) {
    // The names that changed were lost. Re-open children so a replacement at
    // the same name gets a watch on the new directory.
    std::vector<std::filesystem::path> children;
    for (const auto& [name, child] : m_watches) {
      if (!name.empty() && name.parent_path() == watch.relative) {
        children.push_back(name);
      }
    }
    for (const auto& child : children) {
      retire_watches(child);
    }
    watch_children(watch);
  } else {
    bool changed_subtree = false;
    for (const auto& path : renamed) {
      changed_subtree |= m_watches.contains(path);
      retire_watches(path);
      add_watch(path);
      changed_subtree |= m_watches.contains(path);
    }
    // Files may have changed before a newly attached directory's watch was
    // armed, or a directory full of previously tracked inputs may have gone.
    if (changed_subtree) {
      diff = DirectoryTreeDiff{.everything_dirty = true};
    }
  }
  if (diff.everything_dirty || !diff.entries_changed.empty() ||
      !diff.child_lists_changed.empty()) {
    notify_subscribers(diff);
  }
}

void RealDirectoryTree::Impl::notify_subscribers(
    const DirectoryTreeDiff& diff) const {
  const std::lock_guard notifying(m_notifying);
  std::vector<std::function<void(const DirectoryTreeDiff&)>> callbacks = [&] {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<std::function<void(const DirectoryTreeDiff&)>>(
        m_subscribers.cbegin(), m_subscribers.cend());
  }();

  for (const std::function<void(const DirectoryTreeDiff&)>& cb : callbacks) {
    cb(diff);
  }
}

RealDirectoryTree::RealDirectoryTree(const std::filesystem::path& root,
                                     IoContext& io,
                                     BuildPaths excluded)
    : m_impl(std::make_unique<Impl>(root, io, std::move(excluded))) {}

RealDirectoryTree::~RealDirectoryTree() = default;

std::expected<EntryInfo, std::error_code> RealDirectoryTree::status(
    const std::filesystem::path& path) const {
  return m_impl->status(path);
}

std::expected<std::vector<TreeEntry>, std::error_code> RealDirectoryTree::ls(
    const std::filesystem::path& path) const {
  return m_impl->ls(path);
}

OpenSender RealDirectoryTree::open(const std::filesystem::path& path,
                                   const OpenContext&) const {
  return DirectoryTreeUtil::open_by_status(*this, path);
}

ReadSender RealDirectoryTree::read(const std::filesystem::path& path,
                                   Offset offset,
                                   std::size_t size) const {
  return stdexec::just() | stdexec::then([this, path, offset, size] {
           return m_impl->read(path, offset, size);
         });
}

std::expected<FileContent, std::error_code> RealDirectoryTree::Impl::read_whole(
    const std::filesystem::path& path) const {
  const std::optional<std::filesystem::path> resolved = resolve(path);
  if (!resolved.has_value()) {
    return std::unexpected(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  // Shared every way, as read() is, for the same reason.
  const HANDLE file =
      CreateFileW(resolved->c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::unexpected(last_error_code());
  }
  const ScopeGuard close_file([file] { CloseHandle(file); });

  FileContent latest;
  for (int attempt = 0; attempt < k_read_whole_attempts; ++attempt) {
    BY_HANDLE_FILE_INFORMATION before{};
    if (!GetFileInformationByHandle(file, &before)) {
      return std::unexpected(last_error_code());
    }
    if ((before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return std::unexpected(std::make_error_code(std::errc::is_a_directory));
    }
    const std::uint64_t size =
        combine(before.nFileSizeHigh, before.nFileSizeLow);

    // One byte more than the file should hold, so a read that fills the
    // buffer is never mistaken for the end of it.
    std::string contents(static_cast<std::size_t>(size) + 1, '\0');
    std::size_t total = 0;
    for (;;) {
      if (total == contents.size()) {
        contents.resize(contents.size() * 2);
      }
      // A positioned read on a synchronous handle, as in read().
      const auto position = static_cast<std::uint64_t>(total);
      OVERLAPPED overlapped{};
      overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFULL);
      overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
      const auto chunk = static_cast<DWORD>(
          std::min<std::uint64_t>(contents.size() - total, k_read_chunk_bytes));
      DWORD read_bytes = 0;
      if (!ReadFile(file, contents.data() + total, chunk, &read_bytes,
                    &overlapped)) {
        if (GetLastError() == ERROR_HANDLE_EOF) {
          break;
        }
        return std::unexpected(last_error_code());
      }
      if (read_bytes == 0) {
        break;
      }
      total += read_bytes;
    }
    contents.resize(total);

    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(file, &after)) {
      return std::unexpected(last_error_code());
    }
    latest = {
        .bytes = std::make_shared<const std::string>(std::move(contents)),
        .info = {.size = total, .mtime = to_file_time(before.ftLastWriteTime)}};
    if (same_version(before, after) && total == size) {
      break;
    }
  }
  return latest;
}

ReadWholeSender RealDirectoryTree::read_whole(
    const std::filesystem::path& path) const {
  return stdexec::just() |
         stdexec::then([this, path] { return m_impl->read_whole(path); });
}

Subscription RealDirectoryTree::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  return m_impl->subscribe_to_changes(callback);
}

}  // namespace makebelieve
