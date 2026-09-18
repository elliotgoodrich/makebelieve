// SPDX-License-Identifier: MIT
#include "virtualfilesystem.hpp"

#include "directorytree.hpp"
#include "inmemorydirectorytree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

// The bound on every asynchronous effect below. Generous, because it only has
// to be reached when something is actually broken.
constexpr std::chrono::milliseconds k_timeout = 10s;

// Returns an empty path on failure so the caller can ASSERT.
std::filesystem::path make_temp_directory() {
  std::random_device entropy;
  for (int attempt = 0; attempt < 5; ++attempt) {
    const std::filesystem::path candidate =
        std::filesystem::temp_directory_path() /
        ("makebelieve_vfs_" + std::to_string(entropy()) + "_" +
         std::to_string(entropy()));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error) && !error) {
      return candidate;
    }
  }
  return {};
}

// The whole file through an ordinary stream, or nullopt if it could not be
// opened or a read failed part way.
std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string content;
  std::array<char, 4096> chunk{};
  while (in.read(chunk.data(), chunk.size()) || in.gcount() > 0) {
    content.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
  }
  if (in.bad()) {
    return std::nullopt;
  }
  return content;
}

// Reads what is left of @a in from its current position.
std::string read_rest(std::ifstream& in) {
  std::string content;
  std::array<char, 4096> chunk{};
  while (in.read(chunk.data(), chunk.size()) || in.gcount() > 0) {
    content.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
  }
  return content;
}

// Names as UTF-8 strings: std::filesystem::path is itself a range of paths,
// which GoogleTest cannot print, and the order directory_iterator yields is
// unspecified.
std::string utf8(const std::filesystem::path& path) {
  const std::u8string name = path.u8string();
  return {name.begin(), name.end()};
}

std::vector<std::string> sorted_listing(const std::filesystem::path& dir) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    names.push_back(utf8(entry.path().filename()));
  }
  std::ranges::sort(names);
  return names;
}

// Content big enough to take several reads to drain on either backend (FUSE
// asks for at most 128KiB at a time, WinFsp for 64KiB), and patterned so a
// chunk served at the wrong offset cannot go unnoticed.
std::string large_contents() {
  std::string content(300'000, '\0');
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>('a' + ((i * 7 + (i >> 11)) % 26));
  }
  return content;
}

// A timestamp both backends can carry exactly (MSVC's file_clock ticks in
// 100ns), with a fractional part so truncation to whole seconds shows.
std::chrono::file_clock::time_point fixed_mtime() {
  return std::chrono::floor<std::chrono::seconds>(
             std::chrono::file_clock::now()) -
         1h + 250ms;
}

enum class Change { added, removed, modified };

// The two ways to map a file read-only. Both platforms have both, and programs
// use both: a copy-on-write view is what most loaders and many compilers take.
enum class Mapping { shared, copy_on_write };

// Memory-maps files read-only and copies them out, the way compilers, linkers
// and indexers consume their inputs.
//
// On Linux the mapping is done by a helper process. The mount is served from
// this very process, and a thread faulting on it holds this process's mmap
// lock while it waits on the server - which the server may itself need, so an
// in-process mapping can hang the test for good (observed: an unkillable
// D-state process). A separate process is also simply what a real mapper is.
// Construct one before mounting, so the helper inherits no mount state.
class FileMapper {
 public:
  FileMapper();
  ~FileMapper();
  FileMapper(const FileMapper&) = delete;
  FileMapper& operator=(const FileMapper&) = delete;
  FileMapper(FileMapper&&) = delete;
  FileMapper& operator=(FileMapper&&) = delete;

  std::expected<std::string, std::error_code> map(
      const std::filesystem::path& path,
      Mapping mapping_kind);

#ifndef _WIN32
 private:
  pid_t m_pid = -1;
  int m_requests = -1;
  int m_replies = -1;
#endif
};

// Watches one directory for change notifications through the platform's own
// API, the way an editor or build tool watching the mount would. The APIs have
// nothing in common, which is why this is the one place split per platform.
class ChangeWatcher {
 public:
  explicit ChangeWatcher(const std::filesystem::path& directory);
  ~ChangeWatcher();
  ChangeWatcher(const ChangeWatcher&) = delete;
  ChangeWatcher& operator=(const ChangeWatcher&) = delete;
  ChangeWatcher(ChangeWatcher&&) = delete;
  ChangeWatcher& operator=(ChangeWatcher&&) = delete;

  [[nodiscard]] bool armed() const;

  // Waits until a notification of kind @a change (any kind if nullopt) names
  // the leaf @a name, or @a timeout passes.
  bool wait_for(const std::filesystem::path& name,
                std::optional<Change> change,
                std::chrono::milliseconds timeout = k_timeout);

 private:
  struct Event {
    std::filesystem::path name;
    Change change;
  };

  [[nodiscard]] bool seen(const std::filesystem::path& name,
                          std::optional<Change> change) const {
    return std::ranges::any_of(m_events, [&](const Event& event) {
      return event.name == name &&
             (!change.has_value() || event.change == *change);
    });
  }

  // Blocks up to @a timeout for another batch of events, appending them to
  // m_events. Returns false once the watch is broken.
  bool pump(std::chrono::milliseconds timeout);

  std::vector<Event> m_events;
#ifdef _WIN32
  bool rearm();

  HANDLE m_directory = INVALID_HANDLE_VALUE;
  HANDLE m_event = nullptr;
  OVERLAPPED m_overlapped{};
  bool m_pending = false;
  // DWORD elements, because ReadDirectoryChangesW wants DWORD-aligned storage.
  std::array<DWORD, 4096> m_buffer{};
#else
  int m_fd = -1;
#endif
};

bool ChangeWatcher::wait_for(const std::filesystem::path& name,
                             std::optional<Change> change,
                             std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!seen(name, change)) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining <= 0ms || !pump(remaining)) {
      return false;
    }
  }
  return true;
}

#ifdef _WIN32

ChangeWatcher::ChangeWatcher(const std::filesystem::path& directory) {
  m_directory =
      ::CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  m_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_overlapped = {.hEvent = m_event};
  if (m_directory != INVALID_HANDLE_VALUE && m_event != nullptr) {
    rearm();
  }
}

ChangeWatcher::~ChangeWatcher() {
  if (m_pending) {
    ::CancelIoEx(m_directory, &m_overlapped);
    DWORD discard = 0;
    ::GetOverlappedResult(m_directory, &m_overlapped, &discard, TRUE);
  }
  if (m_event != nullptr) {
    ::CloseHandle(m_event);
  }
  if (m_directory != INVALID_HANDLE_VALUE) {
    ::CloseHandle(m_directory);
  }
}

bool ChangeWatcher::armed() const {
  return m_pending;
}

bool ChangeWatcher::rearm() {
  ::ResetEvent(m_event);
  // Everything but last-access, so the test's own reads do not count.
  constexpr DWORD k_filter =
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
      FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
  DWORD unused = 0;
  m_pending =
      ::ReadDirectoryChangesW(m_directory, m_buffer.data(),
                              static_cast<DWORD>(sizeof(m_buffer)), FALSE,
                              k_filter, &unused, &m_overlapped, nullptr) != 0;
  return m_pending;
}

bool ChangeWatcher::pump(std::chrono::milliseconds timeout) {
  if (!m_pending) {
    return false;
  }
  if (::WaitForSingleObject(m_event, static_cast<DWORD>(timeout.count())) !=
      WAIT_OBJECT_0) {
    return true;  // nothing yet; the caller decides whether to keep waiting
  }
  DWORD bytes = 0;
  m_pending = false;
  if (::GetOverlappedResult(m_directory, &m_overlapped, &bytes, FALSE) == 0) {
    return false;
  }
  for (std::size_t offset = 0; offset < bytes;) {
    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
        reinterpret_cast<const BYTE*>(m_buffer.data()) + offset);
    const std::wstring_view name(info->FileName,
                                 info->FileNameLength / sizeof(wchar_t));
    switch (info->Action) {
      case FILE_ACTION_ADDED:
      case FILE_ACTION_RENAMED_NEW_NAME:
        m_events.push_back({name, Change::added});
        break;
      case FILE_ACTION_REMOVED:
      case FILE_ACTION_RENAMED_OLD_NAME:
        m_events.push_back({name, Change::removed});
        break;
      default:
        m_events.push_back({name, Change::modified});
        break;
    }
    if (info->NextEntryOffset == 0) {
      break;
    }
    offset += info->NextEntryOffset;
  }
  return rearm();
}

// Reads the start of @a path with the native API, returning the error the
// open or the read failed with, or an empty error_code if both succeeded.
std::error_code read_error(const std::filesystem::path& path) {
  const HANDLE file =
      ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {static_cast<int>(::GetLastError()), std::system_category()};
  }
  std::array<char, 16> buffer{};
  DWORD bytes = 0;
  std::error_code result;
  if (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes,
                 nullptr) == 0) {
    result = {static_cast<int>(::GetLastError()), std::system_category()};
  }
  ::CloseHandle(file);
  return result;
}

// The platform's own spelling of "access denied", as a tree built on native
// APIs (RealDirectoryTree) would report it.
std::error_code native_access_denied() {
  return {ERROR_ACCESS_DENIED, std::system_category()};
}

FileMapper::FileMapper() = default;
FileMapper::~FileMapper() = default;

// Windows needs no helper process: a fault on the mount does not hold anything
// the WinFsp dispatcher threads need.
std::expected<std::string, std::error_code> FileMapper::map(
    const std::filesystem::path& path,
    Mapping mapping_kind) {
  const bool shared = mapping_kind == Mapping::shared;
  const auto last_error = [] {
    return std::unexpected(std::error_code(static_cast<int>(::GetLastError()),
                                           std::system_category()));
  };
  const HANDLE file =
      ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return last_error();
  }
  std::expected<std::string, std::error_code> result;
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file, &size) == 0) {
    result = last_error();
  } else if (const HANDLE mapping = ::CreateFileMappingW(
                 file, nullptr, shared ? PAGE_READONLY : PAGE_WRITECOPY, 0, 0,
                 nullptr);
             mapping == nullptr) {
    result = last_error();
  } else {
    if (const LPVOID view = ::MapViewOfFile(
            mapping, shared ? FILE_MAP_READ : FILE_MAP_COPY, 0, 0, 0);
        view == nullptr) {
      result = last_error();
    } else {
      result = std::string(static_cast<const char*>(view),
                           static_cast<std::size_t>(size.QuadPart));
      ::UnmapViewOfFile(view);
    }
    ::CloseHandle(mapping);
  }
  ::CloseHandle(file);
  return result;
}

#else

ChangeWatcher::ChangeWatcher(const std::filesystem::path& directory)
    : m_fd(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) {
  // Everything but IN_ACCESS/IN_OPEN/IN_CLOSE_NOWRITE, so the test's own reads
  // do not count.
  if (m_fd >= 0 &&
      ::inotify_add_watch(m_fd, directory.c_str(),
                          IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE |
                              IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO) < 0) {
    ::close(m_fd);
    m_fd = -1;
  }
}

ChangeWatcher::~ChangeWatcher() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

bool ChangeWatcher::armed() const {
  return m_fd >= 0;
}

bool ChangeWatcher::pump(std::chrono::milliseconds timeout) {
  if (m_fd < 0) {
    return false;
  }
  pollfd descriptor{.fd = m_fd, .events = POLLIN, .revents = 0};
  const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  if (ready < 0) {
    return errno == EINTR;
  }
  if (ready == 0) {
    return true;
  }
  alignas(inotify_event) std::array<char, 16384> buffer{};
  const ssize_t count = ::read(m_fd, buffer.data(), buffer.size());
  if (count < 0) {
    return errno == EINTR || errno == EAGAIN;
  }
  for (ssize_t offset = 0; offset < count;) {
    const auto* event =
        reinterpret_cast<const inotify_event*>(buffer.data() + offset);
    if (event->len > 0) {
      Change change = Change::modified;
      if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
        change = Change::added;
      } else if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
        change = Change::removed;
      }
      m_events.push_back({std::filesystem::path(event->name), change});
    }
    offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
  }
  return true;
}

std::error_code read_error(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {errno, std::system_category()};
  }
  std::array<char, 16> buffer{};
  std::error_code result;
  if (::read(fd, buffer.data(), buffer.size()) < 0) {
    result = {errno, std::system_category()};
  }
  ::close(fd);
  return result;
}

std::error_code native_access_denied() {
  return {EACCES, std::system_category()};
}

// The largest path a request carries and file a reply carries.
constexpr std::size_t k_max_request = 4096;
constexpr std::size_t k_max_mapped = 1U << 20;

struct MapRequest {
  std::uint32_t shared;
  std::uint32_t path_size;
};

struct MapReply {
  std::int32_t error;
  std::uint32_t size;
};

bool read_exactly(int fd, char* data, std::size_t size) {
  while (size > 0) {
    const ssize_t count = ::read(fd, data, size);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    data += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

bool write_exactly(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    const ssize_t count = ::write(fd, data, size);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    data += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

// The helper's whole life: serve map requests until the request pipe closes.
// Forked from a multithreaded process, so only async-signal-safe calls and the
// memory allocated before the fork.
[[noreturn]] void serve_maps(int requests, int replies, char* path) {
  MapRequest request{};
  while (read_exactly(requests, reinterpret_cast<char*>(&request),
                      sizeof(request)) &&
         request.path_size < k_max_request &&
         read_exactly(requests, path, request.path_size)) {
    path[request.path_size] = '\0';
    MapReply reply{};
    void* view = MAP_FAILED;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    struct stat info {};
    if (fd < 0 || ::fstat(fd, &info) != 0) {
      reply = {.error = errno};
    } else if (static_cast<std::size_t>(info.st_size) > k_max_mapped) {
      reply = {.error = EFBIG};
    } else {
      const auto size = static_cast<std::uint32_t>(info.st_size);
      view = ::mmap(nullptr, size, PROT_READ,
                    request.shared != 0 ? MAP_SHARED : MAP_PRIVATE, fd, 0);
      reply = view == MAP_FAILED ? MapReply{.error = errno}
                                 : MapReply{.size = size};
    }
    // Copying out of the view is what faults the pages in, here in the helper.
    const bool sent =
        write_exactly(replies, reinterpret_cast<const char*>(&reply),
                      sizeof(reply)) &&
        (view == MAP_FAILED ||
         write_exactly(replies, static_cast<const char*>(view), reply.size));
    if (view != MAP_FAILED) {
      ::munmap(view, reply.size);
    }
    if (fd >= 0) {
      ::close(fd);
    }
    if (!sent) {
      break;
    }
  }
  ::_exit(0);
}

FileMapper::FileMapper() {
  std::array<int, 2> requests{-1, -1};
  std::array<int, 2> replies{-1, -1};
  if (::pipe2(requests.data(), O_CLOEXEC) != 0 ||
      ::pipe2(replies.data(), O_CLOEXEC) != 0) {
    return;
  }
  // Allocated before the fork: the helper must not allocate.
  std::vector<char> path(k_max_request + 1);
  m_pid = ::fork();
  if (m_pid == 0) {
    // Otherwise the helper holds its own request pipe open and never sees the
    // end of it.
    ::close(requests[1]);
    ::close(replies[0]);
    serve_maps(requests[0], replies[1], path.data());
  }
  ::close(requests[0]);
  ::close(replies[1]);
  m_requests = requests[1];
  m_replies = replies[0];
}

FileMapper::~FileMapper() {
  if (m_requests >= 0) {
    ::close(m_requests);  // tells the helper to exit
  }
  if (m_replies >= 0) {
    ::close(m_replies);
  }
  if (m_pid > 0) {
    int status = 0;
    while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
}

std::expected<std::string, std::error_code> FileMapper::map(
    const std::filesystem::path& path,
    Mapping mapping_kind) {
  const auto broken = [] {
    return std::unexpected(std::make_error_code(std::errc::broken_pipe));
  };
  const std::string& name = path.native();
  const MapRequest request{
      .shared = mapping_kind == Mapping::shared ? 1U : 0U,
      .path_size = static_cast<std::uint32_t>(name.size())};
  if (m_pid <= 0 || name.size() >= k_max_request ||
      !write_exactly(m_requests, reinterpret_cast<const char*>(&request),
                     sizeof(request)) ||
      !write_exactly(m_requests, name.data(), name.size())) {
    return broken();
  }
  MapReply reply{};
  if (!read_exactly(m_replies, reinterpret_cast<char*>(&reply),
                    sizeof(reply))) {
    return broken();
  }
  if (reply.error != 0) {
    return std::unexpected(
        std::error_code(reply.error, std::system_category()));
  }
  std::string content(reply.size, '\0');
  if (!read_exactly(m_replies, content.data(), content.size())) {
    return broken();
  }
  return content;
}

#endif

// Forwards to another tree but fails every read with a fixed error, to see
// what a reader of the mount is told when the tree cannot serve a file.
class FailingReadTree : public makebelieve::DirectoryTree {
  const makebelieve::DirectoryTree& m_inner;
  std::error_code m_error;

 public:
  FailingReadTree(const makebelieve::DirectoryTree& inner,
                  std::error_code error)
      : m_inner(inner), m_error(error) {}

  [[nodiscard]] std::expected<makebelieve::EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override {
    return m_inner.status(path);
  }

  [[nodiscard]] std::expected<std::vector<makebelieve::TreeEntry>,
                              std::error_code>
  ls(const std::filesystem::path& path) const override {
    return m_inner.ls(path);
  }

  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& /*path*/,
      makebelieve::Offset /*offset*/,
      std::size_t /*size*/) const override {
    return std::unexpected(m_error);
  }

  [[nodiscard]] makebelieve::Subscription subscribe_to_changes(
      const std::function<void(const makebelieve::DirectoryTreeDiff&)>&
          callback) const override {
    return m_inner.subscribe_to_changes(callback);
  }
};

// Forwards to an in-memory tree, but open() writes a file's final contents
// first, like BuildDirectoryTree. Counts its opens.
class SettleOnOpenTree : public makebelieve::DirectoryTree {
  makebelieve::InMemoryDirectoryTree& m_inner;
  std::string m_final;
  mutable std::atomic<int> m_opens = 0;

 public:
  SettleOnOpenTree(makebelieve::InMemoryDirectoryTree& inner, std::string final)
      : m_inner(inner), m_final(std::move(final)) {}

  [[nodiscard]] int opens() const { return m_opens; }

  [[nodiscard]] std::expected<makebelieve::EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override {
    return m_inner.status(path);
  }

  [[nodiscard]] std::expected<std::vector<makebelieve::TreeEntry>,
                              std::error_code>
  ls(const std::filesystem::path& path) const override {
    return m_inner.ls(path);
  }

  [[nodiscard]] std::expected<makebelieve::FileInfo, std::error_code> open(
      const std::filesystem::path& path) const override {
    ++m_opens;
    m_inner.write_file(path, m_final);
    return DirectoryTree::open(path);
  }

  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      makebelieve::Offset offset,
      std::size_t size) const override {
    return m_inner.read(path, offset, size);
  }

  [[nodiscard]] makebelieve::Subscription subscribe_to_changes(
      const std::function<void(const makebelieve::DirectoryTreeDiff&)>&
          callback) const override {
    return m_inner.subscribe_to_changes(callback);
  }
};

// Forwards to an in-memory tree, but holds open() of one path until released.
class GatedOpenTree : public makebelieve::DirectoryTree {
  const makebelieve::DirectoryTree& m_inner;
  std::filesystem::path m_gated;
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_changed;
  mutable bool m_waiting = false;
  bool m_released = false;

 public:
  GatedOpenTree(const makebelieve::DirectoryTree& inner,
                std::filesystem::path gated)
      : m_inner(inner), m_gated(std::move(gated)) {}

  // Waits (bounded) until an open() of the gated path is held.
  [[nodiscard]] bool wait_until_held() const {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, k_timeout, [this] { return m_waiting; });
  }

  void release() {
    {
      const std::lock_guard lock(m_mutex);
      m_released = true;
    }
    m_changed.notify_all();
  }

  [[nodiscard]] std::expected<makebelieve::EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override {
    return m_inner.status(path);
  }

  [[nodiscard]] std::expected<std::vector<makebelieve::TreeEntry>,
                              std::error_code>
  ls(const std::filesystem::path& path) const override {
    return m_inner.ls(path);
  }

  [[nodiscard]] std::expected<makebelieve::FileInfo, std::error_code> open(
      const std::filesystem::path& path) const override {
    if (path == m_gated) {
      std::unique_lock lock(m_mutex);
      m_waiting = true;
      m_changed.notify_all();
      m_changed.wait(lock, [this] { return m_released; });
    }
    return m_inner.open(path);
  }

  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      makebelieve::Offset offset,
      std::size_t size) const override {
    return m_inner.read(path, offset, size);
  }

  [[nodiscard]] makebelieve::Subscription subscribe_to_changes(
      const std::function<void(const makebelieve::DirectoryTreeDiff&)>&
          callback) const override {
    return m_inner.subscribe_to_changes(callback);
  }
};

// Named for the type under test so TEST_F reads as VirtualFileSystem.<case>.
// That takes the name, so the class under test is spelled makebelieve::
// throughout this file.
class VirtualFileSystem : public ::testing::Test {
 protected:
  void SetUp() override {
    m_scratch = make_temp_directory();
    ASSERT_FALSE(m_scratch.empty()) << "could not create a temp directory";
    m_mountpoint = m_scratch / "mnt";
  }

  void TearDown() override {
    m_vfs.reset();
    std::error_code ignored;
    std::filesystem::remove_all(m_scratch, ignored);
  }

  void mount() { mount_at(m_mountpoint); }
  void mount_at(const std::filesystem::path& mountpoint) {
    m_vfs.emplace(m_tree, mountpoint);
  }
  void unmount() { m_vfs.reset(); }

  makebelieve::InMemoryDirectoryTree& tree() { return m_tree; }
  const std::filesystem::path& scratch() const { return m_scratch; }
  const std::filesystem::path& mountpoint() const { return m_mountpoint; }

 private:
  std::filesystem::path m_scratch;
  std::filesystem::path m_mountpoint;
  // Declared before m_vfs so it outlives the mount, as the contract requires.
  makebelieve::InMemoryDirectoryTree m_tree;
  std::optional<makebelieve::VirtualFileSystem> m_vfs;
};

// --- Mountpoint handling ---------------------------------------------------

// The contract refuses to reuse a path, and a refused path must be left alone:
// the constructor has no business removing something it did not create.
TEST_F(VirtualFileSystem, RefusesAnExistingMountpointAndLeavesItAlone) {
  ASSERT_TRUE(std::filesystem::create_directory(mountpoint()));
  std::ofstream(mountpoint() / "keep.txt") << "keep";
  EXPECT_THROW(mount(), std::filesystem::filesystem_error);
  EXPECT_EQ(read_file(mountpoint() / "keep.txt"), "keep");

  const std::filesystem::path file = scratch() / "file";
  std::ofstream(file) << "file";
  EXPECT_THROW(mount_at(file), std::filesystem::filesystem_error);
  EXPECT_EQ(read_file(file), "file");
}

// A mountpoint beneath a regular file can never be created, and the contract
// names filesystem_error for that rather than any system_error.
TEST_F(VirtualFileSystem, RefusesAMountpointThatCannotBeCreated) {
  const std::filesystem::path file = scratch() / "file";
  std::ofstream(file) << "file";
  EXPECT_THROW(mount_at(file / "mnt"), std::filesystem::filesystem_error);
  EXPECT_EQ(read_file(file), "file");
}

// Missing parents are created, and destruction removes the mountpoint - only
// the mountpoint - without leaving the path unusable for the next mount.
TEST_F(VirtualFileSystem, CreatesParentsAndRemovesTheMountpointOnDestruction) {
  tree().write_file("a.txt", "abc");
  const std::filesystem::path nested = scratch() / "x" / "y" / "mnt";
  for (int round = 0; round < 3; ++round) {
    SCOPED_TRACE(round);
    mount_at(nested);
    EXPECT_EQ(read_file(nested / "a.txt"), "abc");
    unmount();
    EXPECT_FALSE(std::filesystem::exists(nested));
    EXPECT_TRUE(std::filesystem::is_directory(nested.parent_path()));
  }
}

// The daemon passes its command-line argument straight through, so `mnt/` is
// as likely a spelling as `mnt`.
TEST_F(VirtualFileSystem, AcceptsAMountpointWithATrailingSeparator) {
  tree().write_file("a.txt", "abc");
  ASSERT_NO_THROW(mount_at(mountpoint() / ""));
  EXPECT_EQ(read_file(mountpoint() / "a.txt"), "abc");
  unmount();
  EXPECT_FALSE(std::filesystem::exists(mountpoint()));
}

// Both backends absolutize the mountpoint because the working directory can
// move on while the mount lives; teardown must still find what it created.
TEST_F(VirtualFileSystem, ResolvesARelativeMountpointAgainstTheStartingCwd) {
  tree().write_file("a.txt", "abc");
  const std::filesystem::path original = std::filesystem::current_path();
  std::filesystem::current_path(scratch());
  std::optional<std::string> content;
  try {
    mount_at("relative");
    std::filesystem::current_path(original);
    content = read_file(scratch() / "relative" / "a.txt");
    unmount();
  } catch (...) {
    std::filesystem::current_path(original);
    throw;
  }
  EXPECT_EQ(content, "abc");
  EXPECT_FALSE(std::filesystem::exists(scratch() / "relative"));
}

// --- What the mount shows --------------------------------------------------

TEST_F(VirtualFileSystem, ListsEntriesWithTheirKindsSizesAndTimes) {
  const auto mtime = fixed_mtime();
  tree().write_file("a.txt", "abc", mtime);
  tree().write_file("empty.txt", "", mtime);
  tree().make_directory("sub", mtime);
  tree().write_file("sub/nested.txt", "nested", mtime);
  tree().make_directory("sub/deeper", mtime);
  mount();

  EXPECT_EQ(sorted_listing(mountpoint()),
            (std::vector<std::string>{"a.txt", "empty.txt", "sub"}));
  EXPECT_EQ(sorted_listing(mountpoint() / "sub"),
            (std::vector<std::string>{"deeper", "nested.txt"}));
  EXPECT_TRUE(sorted_listing(mountpoint() / "sub" / "deeper").empty());

  EXPECT_TRUE(std::filesystem::is_regular_file(mountpoint() / "a.txt"));
  EXPECT_TRUE(std::filesystem::is_directory(mountpoint() / "sub"));
  EXPECT_EQ(std::filesystem::file_size(mountpoint() / "a.txt"), 3U);
  EXPECT_EQ(std::filesystem::file_size(mountpoint() / "empty.txt"), 0U);
  EXPECT_EQ(std::filesystem::file_size(mountpoint() / "sub" / "nested.txt"),
            6U);

  EXPECT_EQ(std::filesystem::last_write_time(mountpoint() / "a.txt"), mtime);
  EXPECT_EQ(std::filesystem::last_write_time(mountpoint() / "sub"), mtime);

  // Files advertise that they are read-only on both platforms.
  const auto perms =
      std::filesystem::status(mountpoint() / "a.txt").permissions();
  EXPECT_EQ(perms & std::filesystem::perms::owner_write,
            std::filesystem::perms::none);
  EXPECT_NE(perms & std::filesystem::perms::owner_read,
            std::filesystem::perms::none);
}

// Enough entries that neither backend can hand the listing back in one
// buffer, so resuming part way through has to neither skip nor repeat.
TEST_F(VirtualFileSystem, ListsALargeDirectoryCompletely) {
  std::vector<std::string> expected;
  for (int i = 0; i < 2000; ++i) {
    std::string name = "file_with_a_longish_name_" + std::to_string(i) + ".txt";
    tree().write_file(name, "x");
    expected.push_back(std::move(name));
  }
  std::ranges::sort(expected);
  mount();
  EXPECT_EQ(sorted_listing(mountpoint()), expected);
}

TEST_F(VirtualFileSystem, ServesContentAcrossManyReads) {
  const std::string large = large_contents();
  const std::string binary{'\r', '\n', '\0', static_cast<char>(0xff)};
  tree().write_file("large.bin", large);
  tree().write_file("binary.bin", binary);
  tree().write_file("empty.txt", "");
  mount();

  EXPECT_EQ(read_file(mountpoint() / "large.bin"), large);
  EXPECT_EQ(read_file(mountpoint() / "binary.bin"), binary);
  EXPECT_EQ(read_file(mountpoint() / "empty.txt"), "");

  // Reads that start at or past the end yield nothing rather than failing.
  std::ifstream in(mountpoint() / "large.bin", std::ios::binary);
  in.seekg(static_cast<std::streamoff>(large.size() - 5));
  EXPECT_EQ(read_rest(in), large.substr(large.size() - 5));
  in.clear();
  in.seekg(static_cast<std::streamoff>(large.size() + 100'000));
  EXPECT_EQ(read_rest(in), "");
  EXPECT_FALSE(in.bad());
}

TEST_F(VirtualFileSystem, ServesNamesWithSpacesAndUnicode) {
  const std::filesystem::path spaced = u8"with space.txt";
  const std::filesystem::path accented = u8"ünïcødé ✓.txt";
  const std::filesystem::path directory = u8"日本";
  const std::filesystem::path astral = u8"\U0001F600.txt";
  tree().write_file(spaced, "spaced");
  tree().write_file(accented, "accented");
  tree().make_directory(directory);
  tree().write_file(directory / astral, "astral");
  mount();

  std::vector<std::string> expected{utf8(spaced), utf8(accented),
                                    utf8(directory)};
  std::ranges::sort(expected);
  EXPECT_EQ(sorted_listing(mountpoint()), expected);
  EXPECT_EQ(sorted_listing(mountpoint() / directory),
            std::vector<std::string>{utf8(astral)});
  EXPECT_EQ(read_file(mountpoint() / spaced), "spaced");
  EXPECT_EQ(read_file(mountpoint() / accented), "accented");
  EXPECT_EQ(read_file(mountpoint() / directory / astral), "astral");
}

TEST_F(VirtualFileSystem, MissingEntriesAreNotFound) {
  tree().write_file("a.txt", "abc");
  mount();

  std::error_code error;
  EXPECT_FALSE(std::filesystem::exists(mountpoint() / "missing.txt", error));
  EXPECT_FALSE(error) << error.message();
  EXPECT_FALSE(
      std::filesystem::exists(mountpoint() / "missing" / "deeper.txt", error));
  EXPECT_FALSE(error) << error.message();
  EXPECT_FALSE(read_file(mountpoint() / "missing.txt").has_value());
  EXPECT_THROW(std::filesystem::directory_iterator(mountpoint() / "missing"),
               std::filesystem::filesystem_error);
}

// The projection is read-only however it is approached, and a refused write
// must leave what the tree serves untouched.
TEST_F(VirtualFileSystem, RefusesEveryKindOfWrite) {
  tree().write_file("a.txt", "abc");
  tree().make_directory("sub");
  mount();
  const std::filesystem::path file = mountpoint() / "a.txt";

  EXPECT_FALSE(std::ofstream(file, std::ios::binary | std::ios::app));
  EXPECT_FALSE(
      std::ofstream(file, std::ios::binary | std::ios::in | std::ios::out));
  EXPECT_FALSE(std::ofstream(file, std::ios::binary));
  EXPECT_FALSE(std::ofstream(mountpoint() / "new.txt", std::ios::binary));

  std::error_code error;
  EXPECT_FALSE(std::filesystem::create_directory(mountpoint() / "d", error));
  EXPECT_TRUE(error);
  EXPECT_FALSE(std::filesystem::remove(file, error));
  EXPECT_TRUE(error);
  // A directory, unlike a file, carries no read-only attribute on Windows, so
  // this is the removal nothing but the backend itself can refuse.
  EXPECT_FALSE(std::filesystem::remove(mountpoint() / "sub", error));
  EXPECT_TRUE(error);
  EXPECT_TRUE(std::filesystem::is_directory(mountpoint() / "sub"));
  std::filesystem::rename(file, mountpoint() / "b.txt", error);
  EXPECT_TRUE(error);
  std::filesystem::resize_file(file, 0, error);
  EXPECT_TRUE(error);

  EXPECT_EQ(read_file(file), "abc");
  EXPECT_EQ(sorted_listing(mountpoint()),
            (std::vector<std::string>{"a.txt", "sub"}));
  EXPECT_TRUE(tree().status("a.txt").has_value());
  EXPECT_FALSE(tree().status("new.txt").has_value());
}

// Opening for read reaches the tree's open() and a stat does not; the handle
// then reports the size open() settled on.
TEST_F(VirtualFileSystem, OpeningAFileSettlesItAndAStatDoesNot) {
  tree().write_file("a.txt", "x");
  const std::string settled = "settled contents";
  const SettleOnOpenTree settling(tree(), settled);
  const makebelieve::VirtualFileSystem vfs(settling, mountpoint());

  EXPECT_EQ(std::filesystem::file_size(mountpoint() / "a.txt"), 1U);
  EXPECT_TRUE(std::filesystem::is_regular_file(mountpoint() / "a.txt"));
  EXPECT_EQ(settling.opens(), 0);

  std::ifstream in(mountpoint() / "a.txt", std::ios::binary);
  ASSERT_TRUE(in);
  EXPECT_GE(settling.opens(), 1);
  in.seekg(0, std::ios::end);
  EXPECT_EQ(in.tellg(), static_cast<std::streamoff>(settled.size()));
  in.seekg(0);
  EXPECT_EQ(read_rest(in), settled);
}

// While one open waits on the tree, other files can still be stat-ed and read.
TEST_F(VirtualFileSystem, AnOpenThatWaitsDoesNotHoldUpOtherRequests) {
  tree().write_file("slow.txt", "slow");
  tree().write_file("fast.txt", "fast");
  GatedOpenTree gated(tree(), "slow.txt");
  const makebelieve::VirtualFileSystem vfs(gated, mountpoint());

  // After the mount, so these finish before it is torn down.
  std::future<std::optional<std::string>> slow = std::async(
      std::launch::async, [&] { return read_file(mountpoint() / "slow.txt"); });
  if (!gated.wait_until_held()) {
    gated.release();  // or slow's destructor would hang
    FAIL() << "the open of slow.txt never reached the tree";
  }

  std::future<std::optional<std::string>> fast =
      std::async(std::launch::async, [&] {
        std::error_code ignored;
        static_cast<void>(
            std::filesystem::file_size(mountpoint() / "fast.txt", ignored));
        return read_file(mountpoint() / "fast.txt");
      });
  const bool fast_finished =
      fast.wait_for(k_timeout) == std::future_status::ready;
  // Released either way, so a failure cannot wedge the mount.
  gated.release();

  EXPECT_TRUE(fast_finished) << "a waiting open held up another file";
  EXPECT_EQ(fast.get(), "fast");
  EXPECT_EQ(slow.get(), "slow");
}

// An error the tree reports should reach the reader as that error, whichever
// category the tree used - RealDirectoryTree reports native codes, the
// in-memory trees report std::errc.
TEST_F(VirtualFileSystem, SurfacesTheTreesReadError) {
  tree().write_file("a.txt", "abc");
  const std::array<std::error_code, 2> errors{
      std::make_error_code(std::errc::permission_denied),
      native_access_denied()};
  for (std::size_t i = 0; i < errors.size(); ++i) {
    SCOPED_TRACE(errors[i].category().name());
    const FailingReadTree failing(tree(), errors[i]);
    const std::filesystem::path at =
        scratch() / ("failing" + std::to_string(i));
    const makebelieve::VirtualFileSystem vfs(failing, at);
    const std::error_code error = read_error(at / "a.txt");
    EXPECT_TRUE(error == std::errc::permission_denied)
        << error.value() << ": " << error.message();
  }
}

// Compilers, linkers and indexers map their inputs rather than read them, and a
// shared read-only view is the plain way to ask for one.
TEST_F(VirtualFileSystem, ServesSharedMemoryMappings) {
  const std::string large = large_contents();
  tree().write_file("large.bin", large);
  FileMapper mapper;
  mount();

  const std::expected<std::string, std::error_code> mapped =
      mapper.map(mountpoint() / "large.bin", Mapping::shared);
  ASSERT_TRUE(mapped.has_value()) << mapped.error().message();
  EXPECT_TRUE(*mapped == large);
}

#ifdef _WIN32
// Windows lets a volume declare itself case-sensitive or not, and programs
// rely on the declaration; whichever it is, lookups have to honour it. Linux
// has no such declaration - case is always significant there.
TEST_F(VirtualFileSystem, WindowsLookupMatchesTheDeclaredCaseSensitivity) {
  tree().make_directory("Sub");
  tree().write_file("Sub/Mixed.txt", "mixed");
  mount();

  const HANDLE root = ::CreateFileW(
      mountpoint().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  ASSERT_NE(root, INVALID_HANDLE_VALUE);
  DWORD flags = 0;
  const BOOL queried = ::GetVolumeInformationByHandleW(
      root, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0);
  ::CloseHandle(root);
  ASSERT_NE(queried, 0);

  const bool case_sensitive = (flags & FILE_CASE_SENSITIVE_SEARCH) != 0;
  EXPECT_EQ(read_file(mountpoint() / "sub" / "mixed.TXT").has_value(),
            !case_sensitive)
      << "volume declares itself case-"
      << (case_sensitive ? "sensitive" : "insensitive");
}
#endif

// --- Staying in sync with the tree -----------------------------------------

TEST_F(VirtualFileSystem, ReflectsTreeChangesAfterMounting) {
  tree().write_file("a.txt", "abc");
  mount();
  ASSERT_EQ(read_file(mountpoint() / "a.txt"), "abc");

  tree().write_file("a.txt", "a longer value");
  EXPECT_EQ(std::filesystem::file_size(mountpoint() / "a.txt"), 14U);
  EXPECT_EQ(read_file(mountpoint() / "a.txt"), "a longer value");

  // Same size, later timestamp: nothing but the content says it changed.
  tree().write_file("a.txt", "A LONGER VALUE",
                    std::chrono::file_clock::now() + 1s);
  EXPECT_EQ(read_file(mountpoint() / "a.txt"), "A LONGER VALUE");

  tree().write_file("a.txt", "");
  EXPECT_EQ(read_file(mountpoint() / "a.txt"), "");

  tree().make_directory("sub");
  tree().write_file("sub/new.txt", "new");
  EXPECT_EQ(sorted_listing(mountpoint()),
            (std::vector<std::string>{"a.txt", "sub"}));
  EXPECT_EQ(read_file(mountpoint() / "sub" / "new.txt"), "new");

  tree().remove("sub");
  tree().remove("a.txt");
  EXPECT_TRUE(sorted_listing(mountpoint()).empty());
  EXPECT_FALSE(std::filesystem::exists(mountpoint() / "a.txt"));
  EXPECT_FALSE(std::filesystem::exists(mountpoint() / "sub" / "new.txt"));
}

// OpenFile deliberately keeps the path rather than a snapshot, so that a
// reader holding a file open while the build fills it in sees the result.
TEST_F(VirtualFileSystem, OpenHandleSeesTheTreeAsItIsNow) {
  tree().write_file("a.txt", "old");
  mount();
  std::ifstream in(mountpoint() / "a.txt", std::ios::binary);
  ASSERT_TRUE(in);
  ASSERT_EQ(read_rest(in), "old");

  tree().write_file("a.txt", "new and longer",
                    std::chrono::file_clock::now() + 1s);
  in.clear();
  in.seekg(0);
  EXPECT_EQ(read_rest(in), "new and longer");

  tree().write_file("a.txt", "NEW AND LONGER",
                    std::chrono::file_clock::now() + 2s);
  in.clear();
  in.seekg(0);
  EXPECT_EQ(read_rest(in), "NEW AND LONGER");
}

// --- Change notifications --------------------------------------------------

// The basic promise, and that after the notification the file really reads
// back as the tree has it - including emptied, which on Linux is written over
// by the one-byte notification poke.
TEST_F(VirtualFileSystem, NotifiesWhenAFileChanges) {
  tree().write_file("a.txt", "abc");
  mount();
  ASSERT_EQ(read_file(mountpoint() / "a.txt"), "abc");

  for (const std::string content : {"changed", ""}) {
    SCOPED_TRACE("a.txt <- '" + content + "'");
    ChangeWatcher watcher(mountpoint());
    ASSERT_TRUE(watcher.armed());
    tree().write_file("a.txt", content);
    EXPECT_TRUE(watcher.wait_for("a.txt", Change::modified));
    EXPECT_EQ(std::filesystem::file_size(mountpoint() / "a.txt"),
              content.size());
    EXPECT_EQ(read_file(mountpoint() / "a.txt"), content);
  }
}

// Watching the directory a file lives in is the usual way to watch it, and
// most files do not live at the root.
TEST_F(VirtualFileSystem, NotifiesWhenAFileInASubdirectoryChanges) {
  tree().make_directory("sub");
  tree().make_directory("sub/deeper");
  tree().write_file("sub/deeper/nested.txt", "abc");
  mount();
  const std::filesystem::path directory = mountpoint() / "sub" / "deeper";
  ASSERT_EQ(read_file(directory / "nested.txt"), "abc");

  ChangeWatcher watcher(directory);
  ASSERT_TRUE(watcher.armed());
  tree().write_file("sub/deeper/nested.txt", "changed");
  EXPECT_TRUE(watcher.wait_for("nested.txt", Change::modified));
}

// A watcher on a directory hears about every child, including ones nothing has
// looked at through the mount yet - a build output the watcher only knows by
// its listing, say.
TEST_F(VirtualFileSystem, NotifiesForAFileNothingHasOpened) {
  tree().write_file("a.txt", "abc");
  mount();

  ChangeWatcher watcher(mountpoint());
  ASSERT_TRUE(watcher.armed());
  tree().write_file("a.txt", "changed");
  EXPECT_TRUE(watcher.wait_for("a.txt", std::nullopt));
}

// Windows drops a notification for a file with an open handle; the backend
// defers it to the close. The reader here is exactly the shape of a first
// build: the file is held open when the change lands.
TEST_F(VirtualFileSystem, NotifiesForAChangeWhileTheFileIsOpen) {
  tree().write_file("a.txt", "abc");
  mount();

  ChangeWatcher watcher(mountpoint());
  ASSERT_TRUE(watcher.armed());
  {
    std::ifstream in(mountpoint() / "a.txt", std::ios::binary);
    ASSERT_TRUE(in);
    ASSERT_EQ(read_rest(in), "abc");
    std::ifstream second(mountpoint() / "a.txt", std::ios::binary);
    ASSERT_TRUE(second);
    tree().write_file("a.txt", "changed");
  }
  EXPECT_TRUE(watcher.wait_for("a.txt", Change::modified));
  EXPECT_EQ(read_file(mountpoint() / "a.txt"), "changed");
}

// The same, for the other way programs hold on to a file: a mapping, unmapped
// and closed again well before the change lands.
TEST_F(VirtualFileSystem, NotifiesForAFileThatWasMemoryMapped) {
  tree().write_file("a.txt", "abc");
  FileMapper mapper;
  mount();
  ASSERT_EQ(mapper.map(mountpoint() / "a.txt", Mapping::copy_on_write), "abc");

  ChangeWatcher watcher(mountpoint());
  ASSERT_TRUE(watcher.armed());
  tree().write_file("a.txt", "changed");
  EXPECT_TRUE(watcher.wait_for("a.txt", Change::modified));
}

// A watcher learns about new files from an "added" notification, not from a
// "modified" one about a name it has never seen.
TEST_F(VirtualFileSystem, NotifiesWhenAFileIsAdded) {
  mount();
  ChangeWatcher watcher(mountpoint());
  ASSERT_TRUE(watcher.armed());
  tree().write_file("new.txt", "new");
  EXPECT_TRUE(watcher.wait_for("new.txt", Change::added));
}

// A build that stops producing an output has to be able to say so.
TEST_F(VirtualFileSystem, NotifiesWhenAFileIsRemoved) {
  tree().write_file("a.txt", "abc");
  mount();
  ASSERT_EQ(read_file(mountpoint() / "a.txt"), "abc");
  ChangeWatcher watcher(mountpoint());
  ASSERT_TRUE(watcher.armed());
  tree().remove("a.txt");
  EXPECT_TRUE(watcher.wait_for("a.txt", Change::removed));
}

// --- Concurrency and teardown ----------------------------------------------

// Memory mappings, unlike reads, go through the kernel's page cache on both
// platforms, so they are where caching shows. A mapping taken right after a
// change shows exactly what the tree now holds, even when the size stays the
// same and only the timestamp moved - and in particular never anything the
// backend writes to raise the notification for that change, which is in flight
// at the same moment. Each round is one chance to catch that race, hence the
// repetition; a correct backend passes every round.
TEST_F(VirtualFileSystem, MappingAfterAChangeShowsExactlyTheNewContent) {
  const std::filesystem::path file = mountpoint() / "a.txt";
  const auto mtime = fixed_mtime();
  const auto content = [](int i) {
    return std::string(64, static_cast<char>('a' + (i % 26)));
  };
  tree().write_file("a.txt", content(0), mtime);
  FileMapper mapper;
  mount();
  ASSERT_EQ(read_file(file), content(0));

  constexpr int k_rounds = 2000;
  int wrong = 0;
  std::string example;
  for (int i = 1; i <= k_rounds; ++i) {
    tree().write_file("a.txt", content(i), mtime + std::chrono::seconds(i));
    const auto mapped = mapper.map(file, Mapping::copy_on_write);
    if (mapped != content(i) && wrong++ == 0) {
      example =
          mapped.has_value() ? *mapped : "<" + mapped.error().message() + ">";
    }
  }
  EXPECT_EQ(wrong, 0) << "rounds out of " << k_rounds << ", e.g. "
                      << testing::PrintToString(example);
}

// Many handles to the same files overlapping in time. On Windows this is what
// double-freed before file contexts were made per-handle.
TEST_F(VirtualFileSystem, ServesManyConcurrentReaders) {
  const std::string large = large_contents();
  tree().write_file("large.bin", large);
  tree().write_file("small.txt", "small");
  tree().make_directory("sub");
  tree().write_file("sub/nested.txt", "nested");
  mount();

  std::atomic<int> failures{0};
  {
    std::vector<std::jthread> readers;
    for (int t = 0; t < 8; ++t) {
      readers.emplace_back([&] {
        for (int i = 0; i < 25; ++i) {
          std::ifstream held(mountpoint() / "small.txt", std::ios::binary);
          if (read_file(mountpoint() / "large.bin") != large ||
              read_file(mountpoint() / "sub" / "nested.txt") != "nested" ||
              sorted_listing(mountpoint() / "sub") !=
                  std::vector<std::string>{"nested.txt"} ||
              read_rest(held) != "small") {
            ++failures;
          }
        }
      });
    }
  }
  EXPECT_EQ(failures.load(), 0);
}

// Someone else - an editor, a crashed build - holding a file open must not
// keep the daemon from shutting down; the contract promises teardown, and
// the mountpoint gone afterwards.
TEST_F(VirtualFileSystem, TeardownIsNotBlockedByAnOpenHandle) {
  tree().write_file("a.txt", "abc");
  mount();
  std::ifstream held(mountpoint() / "a.txt", std::ios::binary);
  ASSERT_TRUE(held);

  std::promise<void> done;
  std::future<void> finished = done.get_future();
  std::jthread teardown([&] {
    unmount();
    done.set_value();
  });
  const bool unblocked =
      finished.wait_for(k_timeout) == std::future_status::ready;
  // Releasing the handle is what lets a stuck teardown finish, so the test can
  // end either way. Should it still hang, ctest's per-test timeout bounds it.
  held.close();
  teardown.join();

  EXPECT_TRUE(unblocked) << "teardown waited on a handle it does not own";
  EXPECT_FALSE(std::filesystem::exists(mountpoint()));
}

}  // namespace
