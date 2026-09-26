// SPDX-License-Identifier: MIT
#include "realdirectorytree.hpp"

#include "iocontext.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

// Closes a HANDLE on scope exit.
template <BOOL(WINAPI* Close)(HANDLE)>
class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}

  ~ScopedHandle() {
    if (valid()) {
      Close(m_handle);
    }
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&&) = delete;
  ScopedHandle& operator=(ScopedHandle&&) = delete;

  [[nodiscard]] HANDLE get() const { return m_handle; }
  [[nodiscard]] bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }

 private:
  HANDLE m_handle;
};

using UniqueHandle = ScopedHandle<&CloseHandle>;
using UniqueFindHandle = ScopedHandle<&FindClose>;

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
DirectoryTreeDiff parse_changes(const std::byte* buffer, DWORD bytes) {
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
    const auto it = files.emplace(name).first;

    switch (record->Action) {
      case FILE_ACTION_ADDED:
      case FILE_ACTION_REMOVED:
      case FILE_ACTION_RENAMED_OLD_NAME:
      case FILE_ACTION_RENAMED_NEW_NAME:
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

}  // namespace

class RealDirectoryTree::Impl {
  std::filesystem::path m_root;
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

  // The root, opened with FILE_FLAG_OVERLAPPED, and the one
  // ReadDirectoryChangesW read kept outstanding on it: its buffer - on the
  // heap, as overlapped use requires, and DWORD-aligned by coming from the
  // allocator - and the manual-reset event it signals. Created lazily by
  // start_watcher(), then touched only on the context's thread (which issues
  // every read, so none is cancelled by the thread that issued it exiting)
  // until the destructor.
  mutable HANDLE m_directory = INVALID_HANDLE_VALUE;
  mutable HANDLE m_change_event = nullptr;
  mutable OVERLAPPED m_overlapped{};
  mutable std::vector<std::byte> m_buffer;
  mutable bool m_read_pending = false;

  // The wait on m_change_event, stopped and joined on destruction.
  mutable stdexec::counting_scope m_watching;
  mutable bool m_watching_started = false;

 public:
  Impl(const std::filesystem::path& root, IoContext& io)
      : m_root(std::filesystem::weakly_canonical(root)), m_io(io) {}

  ~Impl() {
    if (m_watching_started) {
      m_watching.request_stop();
      stdexec::sync_wait(m_watching.join());
    }
    if (m_read_pending) {
      // The kernel may yet write into m_buffer, so cancel the read and block
      // until the cancellation is acknowledged before releasing it.
      DWORD bytes = 0;
      CancelIoEx(m_directory, &m_overlapped);
      GetOverlappedResult(m_directory, &m_overlapped, &bytes, TRUE);
    }
    if (m_directory != INVALID_HANDLE_VALUE) {
      CloseHandle(m_directory);
    }
    if (m_change_event != nullptr) {
      CloseHandle(m_change_event);
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

  // Issues the next read into m_buffer, returning whether it is outstanding.
  // On the context's thread.
  bool issue_read() const noexcept;

  // Turns the read that just completed into a diff for the subscribers and
  // issues the next. On the context's thread.
  void on_change() const;

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

  return m_root / normal;
}

std::expected<EntryInfo, std::error_code> RealDirectoryTree::Impl::status(
    const std::filesystem::path& path) const {
  // resolve() yields nullopt for an escape and the stat step yields nullopt for
  // an on-disk failure; both collapse to one absent optional, which the tail
  // turns into the single error code the interface promises for "no such
  // entry". A caller that needs to tell the two apart does not exist yet.
  const std::optional<EntryInfo> found = resolve(path).and_then(
      [](const std::filesystem::path& absolute) -> std::optional<EntryInfo> {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(absolute.c_str(), GetFileExInfoStandard,
                                  &data)) {
          return std::nullopt;
        }
        return make_info(data.dwFileAttributes, data.nFileSizeHigh,
                         data.nFileSizeLow, data.ftLastWriteTime);
      });

  if (found.has_value()) {
    return *found;
  }
  return std::unexpected(
      std::make_error_code(std::errc::no_such_file_or_directory));
}

std::expected<std::vector<TreeEntry>, std::error_code>
RealDirectoryTree::Impl::ls(const std::filesystem::path& path) const {
  // and_then runs the enumeration only for a path that stayed inside the root.
  // A present-but-empty vector (an empty directory) is a success; only a failed
  // FindFirstFileExW yields nullopt and therefore the error below.
  const std::optional<std::vector<TreeEntry>> listed =
      resolve(path).and_then([](const std::filesystem::path& absolute)
                                 -> std::optional<std::vector<TreeEntry>> {
        // FindExInfoBasic skips the 8.3 short name, which we never use, and
        // LARGE_FETCH batches directory reads. Both matter for the wide
        // directories a build tree tends to have.
        WIN32_FIND_DATAW data{};
        const std::filesystem::path pattern = absolute / L"*";
        const UniqueFindHandle find(FindFirstFileExW(
            pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
            nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (!find.valid()) {
          return std::nullopt;
        }

        std::vector<TreeEntry> entries;
        do {
          const std::wstring_view name = data.cFileName;
          if (name == L"." || name == L"..") {
            continue;
          }
          entries.emplace_back(
              std::filesystem::path(name),
              make_info(data.dwFileAttributes, data.nFileSizeHigh,
                        data.nFileSizeLow, data.ftLastWriteTime));
        } while (FindNextFileW(find.get(), &data));

        return entries;
      });

  if (listed.has_value()) {
    return *listed;
  }
  return std::unexpected(
      std::make_error_code(std::errc::no_such_file_or_directory));
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
  const UniqueHandle file(
      CreateFileW(resolved->c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) {
    return std::unexpected(last_error_code());
  }

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
      if (!ReadFile(file.get(), data + total, chunk, &read_bytes,
                    &overlapped)) {
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
  // Manual reset, so a change that lands before the wait on it starts is not
  // missed; issue_read() resets it for each read.
  m_change_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (m_change_event == nullptr) {
    throw_last_error("CreateEventW");
  }

  // FILE_LIST_DIRECTORY is the access right ReadDirectoryChangesW needs;
  // BACKUP_SEMANTICS is what allows a directory handle at all.
  m_directory =
      CreateFileW(m_root.c_str(), FILE_LIST_DIRECTORY,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (m_directory == INVALID_HANDLE_VALUE) {
    throw_last_error("CreateFileW (watch root)");
  }

  m_buffer.resize(k_watch_buffer_bytes);

  // Issued, on the context's thread, before this returns, so a change made
  // once the subscription is in place is caught.
  const std::optional issued = stdexec::sync_wait(
      stdexec::schedule(m_io.get_scheduler()) |
      stdexec::then([this]() noexcept { return issue_read(); }));
  if (!issued.has_value() || !std::get<0>(*issued)) {
    throw_last_error("ReadDirectoryChangesW");
  }

  m_io.spawn_watch(m_change_event, m_watching, [this]() noexcept {
    // Best-effort: a change dropped for want of memory is lost, as one the
    // buffer overflows on is.
    try {
      on_change();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
  });
  m_watching_started = true;
}

bool RealDirectoryTree::Impl::issue_read() const noexcept {
  ResetEvent(m_change_event);
  m_overlapped = OVERLAPPED{};
  m_overlapped.hEvent = m_change_event;
  m_read_pending = ReadDirectoryChangesW(m_directory, m_buffer.data(),
                                         static_cast<DWORD>(m_buffer.size()),
                                         TRUE, k_watch_filter, nullptr,
                                         &m_overlapped, nullptr) != 0;
  return m_read_pending;
}

void RealDirectoryTree::Impl::on_change() const {
  DWORD bytes = 0;
  m_read_pending = false;
  if (!GetOverlappedResult(m_directory, &m_overlapped, &bytes, FALSE)) {
    // The watch has failed for good. Leave the event unsignalled so the wait
    // on it stays quiet until the destructor stops it.
    ResetEvent(m_change_event);
    return;
  }

  // bytes == 0 means the buffer overflowed and the kernel discarded the
  // records. The changes still happened but their identities are gone, so
  // there is nothing to hand over but the blanket signal.
  const DirectoryTreeDiff diff =
      bytes == 0 ? DirectoryTreeDiff{.everything_dirty = true}
                 : parse_changes(m_buffer.data(), bytes);

  // The next read goes out before anyone hears of this one, so nothing that
  // changes while they react is missed. A read that will not issue leaves the
  // event reset, quietening the wait as above.
  issue_read();
  notify_subscribers(diff);
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
                                     IoContext& io)
    : m_impl(std::make_unique<Impl>(root, io)) {}

RealDirectoryTree::~RealDirectoryTree() = default;

std::expected<EntryInfo, std::error_code> RealDirectoryTree::status(
    const std::filesystem::path& path) const {
  return m_impl->status(path);
}

std::expected<std::vector<TreeEntry>, std::error_code> RealDirectoryTree::ls(
    const std::filesystem::path& path) const {
  return m_impl->ls(path);
}

std::expected<std::string, std::error_code> RealDirectoryTree::read(
    const std::filesystem::path& path,
    Offset offset,
    std::size_t size) const {
  return m_impl->read(path, offset, size);
}

Subscription RealDirectoryTree::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  return m_impl->subscribe_to_changes(callback);
}

}  // namespace makebelieve
