// SPDX-License-Identifier: MIT
#include "realdirectorytree.hpp"

#include <array>
#include <cerrno>
#include <chrono>
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
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace makebelieve {

namespace {

// Watch mask for entries appearing, disappearing
// or being renamed (which change a directory's child list), plus content and
// attribute changes (which change an entry's own identity). IN_MOVE_SELF and
// IN_DELETE_SELF are implied via the IN_IGNORED that inotify sends when a
// watched directory itself goes away.
constexpr std::uint32_t k_watch_mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                                       IN_MOVED_TO | IN_MODIFY | IN_ATTRIB |
                                       IN_CLOSE_WRITE;

// A single read() can return several coalesced records, so the buffer must
// hold more than one; 64 KiB matches the Windows change buffer and keeps the
// syscall count down under churn.
constexpr std::size_t k_event_buffer_bytes = std::size_t{64} * 1024;

// Return the calling thread's errno as a std::error_code, for the std::expected
// the value-returning methods hand back instead of throwing.
std::error_code last_error_code() {
  return {errno, std::system_category()};
}

// Converts a POSIX timestamp to the file_clock time_point EntryInfo carries.
std::chrono::file_clock::time_point to_file_time(const struct timespec& time) {
  const auto system_time = std::chrono::system_clock::from_time_t(time.tv_sec) +
                           std::chrono::nanoseconds(time.tv_nsec);
  return std::chrono::clock_cast<std::chrono::file_clock>(system_time);
}

// Builds an EntryInfo from info.
EntryInfo make_info(const struct stat& info) {
  if (S_ISDIR(info.st_mode)) {
    return DirectoryInfo{to_file_time(info.st_mtim)};
  }
  return FileInfo{.size = static_cast<std::size_t>(info.st_size),
                  .mtime = to_file_time(info.st_mtim)};
}

using UniqueDir = std::unique_ptr<DIR, int (*)(DIR*)>;

}  // namespace

class RealDirectoryTree::Impl {
  std::filesystem::path m_root;

  // Everything below is mutable because the whole interface is const: these
  // are the internals that const-as-thread-safety permits us to touch, and
  // each is guarded either by m_mutex or by the watcher thread's own lifetime.

  // Guards m_subscribers, m_next_id, and the lazy watcher startup.
  mutable std::mutex m_mutex;
  mutable std::list<std::function<void(const DirectoryTreeDiff&)>>
      m_subscribers;

  // The inotify instance and an eventfd the destructor writes to. Created
  // lazily by start_watcher(). m_watch_descriptors maps each watch descriptor
  // back to its directory relative to the root (the root itself being the
  // empty path), since inotify names only the leaf within the watched
  // directory; it is touched only by start_watcher() and the watcher thread,
  // which never run at the same time, so it needs no lock.
  mutable int m_inotify_fd = -1;
  mutable int m_stop_fd = -1;
  mutable std::map<int, std::filesystem::path> m_watch_descriptors;
  mutable std::thread m_watcher;

 public:
  explicit Impl(const std::filesystem::path& root)
      : m_root(std::filesystem::weakly_canonical(root)) {}

  ~Impl() {
    if (m_watcher.joinable()) {
      // Nudge the eventfd so the watch loop's poll() returns even while
      // blocked, then join before releasing the descriptors it reads.
      const std::uint64_t one = 1;
      const ssize_t written = ::write(m_stop_fd, &one, sizeof(one));
      static_cast<void>(written);
      m_watcher.join();
    }
    if (m_inotify_fd >= 0) {
      ::close(m_inotify_fd);
    }
    if (m_stop_fd >= 0) {
      ::close(m_stop_fd);
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

  // Creates the inotify and eventfd descriptors, plants the initial watches,
  // and spawns the watcher. Called once, from the first subscribe_to_changes(),
  // with m_mutex held.
  void start_watcher() const;

  // The two paths add_watch() needs, named so that a call site cannot pass
  // them the wrong way round - they are the same type and differ only in
  // meaning.
  struct WatchTarget {
    std::filesystem::path absolute;
    std::filesystem::path relative;  // `absolute` relative to the root.
  };

  // Watches `target` and, recursively, every real subdirectory beneath it.
  // inotify is not recursive, so each directory needs its own watch, and new
  // ones are added as directories appear. Symlinked directories are not
  // descended into, which keeps a symlink cycle from looping here.
  //
  // The recursion mirrors the tree, which symlink_status() keeps finite.
  void add_watch(const WatchTarget& target) const;

  // Drains inotify until the eventfd is signalled, turning records into diffs.
  void watch_loop() const;

  // Invokes every registered callback with @a diff. Takes a copy of the
  // callback list so that a callback which unsubscribes cannot deadlock
  // against m_mutex.
  void notify_subscribers(const DirectoryTreeDiff& diff) const;
};

std::optional<std::filesystem::path> RealDirectoryTree::Impl::resolve(
    const std::filesystem::path& path) const {
  if (path.empty()) {
    return m_root;
  }

  // An absolute or root-qualified path would make operator/ discard the root
  // outright, so it is refused before anything else looks at it.
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return std::nullopt;
  }

  // Normalizing first collapses interior "..", so a leading ".." afterwards is
  // exactly the set of paths that climb out of the root.
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
        // stat, not lstat: a symlink inside the root is followed, matching the
        // documented behaviour (and the lexical-only escape guard above).
        struct stat info {};
        if (::stat(absolute.c_str(), &info) != 0) {
          return std::nullopt;
        }
        return make_info(info);
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
  // opendir yields nullopt and therefore the error below.
  const std::optional<std::vector<TreeEntry>> listed =
      resolve(path).and_then([](const std::filesystem::path& absolute)
                                 -> std::optional<std::vector<TreeEntry>> {
        const UniqueDir dir(::opendir(absolute.c_str()), &::closedir);
        if (dir == nullptr) {
          return std::nullopt;
        }
        const int dir_fd = ::dirfd(dir.get());

        std::vector<TreeEntry> entries;
        while (true) {
          // readdir reports end of directory and failure the same way, by
          // returning null, so a cleared errno is the only thing that tells
          // the two apart. It is cleared every iteration rather than once,
          // because the fstatat below leaves one behind for every entry it
          // skips.
          errno = 0;
          const dirent* entry = ::readdir(dir.get());
          if (entry == nullptr) {
            if (errno != 0) {
              return std::nullopt;  // A real failure, not the end.
            }
            break;
          }

          const std::string_view name = entry->d_name;
          if (name == "." || name == "..") {
            continue;
          }
          // fstatat relative to the directory fd avoids rebuilding a full path
          // per entry. An entry that vanished between readdir and here, or is
          // unreachable, is skipped rather than failing the whole listing -
          // the same tolerance the Windows backend has by omitting what it
          // cannot describe.
          struct stat info {};
          if (::fstatat(dir_fd, entry->d_name, &info, 0) != 0) {
            continue;
          }
          entries.push_back(
              {.name = std::filesystem::path(name), .info = make_info(info)});
        }
        return entries;
      });

  if (listed.has_value()) {
    return *listed;
  }
  return std::unexpected(
      std::make_error_code(std::errc::no_such_file_or_directory));
}

// offset and size are the signature DirectoryTree::read() defines, so they
// cannot be reordered or renamed apart here.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::expected<std::string, std::error_code> RealDirectoryTree::Impl::read(
    const std::filesystem::path& path,
    Offset offset,
    std::size_t size) const {
  // NOLINTEND(bugprone-easily-swappable-parameters)
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

  const int fd = ::open(resolved->c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::unexpected(last_error_code());
  }
  // pread throughout, never read/lseek, so the shared descriptor carries no
  // position and concurrent reads of the same path do not race - the same
  // property the Windows backend gets from a positioned OVERLAPPED.
  struct FdCloser {
    int fd;
    ~FdCloser() { ::close(fd); }
  } const closer{fd};

  std::string contents;
  std::optional<std::error_code> failure;
  contents.resize_and_overwrite(size, [&](char* data,
                                          const std::size_t capacity) noexcept {
    std::size_t total = 0;
    while (total < capacity) {
      const ssize_t read_bytes =
          ::pread(fd, data + total, capacity - total,
                  static_cast<off_t>(offset) + static_cast<off_t>(total));
      if (read_bytes < 0) {
        if (errno == EINTR) {
          continue;
        }
        failure = last_error_code();
        break;
      }
      if (read_bytes == 0) {
        break;  // End of file: a short result is how the contract spells EOF.
      }
      total += static_cast<std::size_t>(read_bytes);
    }
    return total;
  });

  if (failure.has_value()) {
    return std::unexpected(*failure);
  }
  return contents;
}

Subscription RealDirectoryTree::Impl::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  const std::lock_guard<std::mutex> lock(m_mutex);

  if (!m_watcher.joinable()) {
    start_watcher();
  }

  const auto it = m_subscribers.emplace(m_subscribers.end(), callback);
  return Subscription([this, it] {
    const std::lock_guard<std::mutex> unsubscribe_lock(m_mutex);
    m_subscribers.erase(it);
  });
}

void RealDirectoryTree::Impl::start_watcher() const {
  m_inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (m_inotify_fd < 0) {
    throw std::system_error(errno, std::system_category(), "inotify_init1");
  }

  // A counter eventfd used purely as a wakeup: the destructor writes to it to
  // break the watch loop's poll().
  m_stop_fd = ::eventfd(0, EFD_CLOEXEC);
  if (m_stop_fd < 0) {
    const int error = errno;
    ::close(m_inotify_fd);
    m_inotify_fd = -1;
    throw std::system_error(error, std::system_category(), "eventfd");
  }

  add_watch({.absolute = m_root, .relative = std::filesystem::path{}});
  m_watcher = std::thread([this]() { watch_loop(); });
}

// NOLINTNEXTLINE(misc-no-recursion): see the declaration.
void RealDirectoryTree::Impl::add_watch(const WatchTarget& target) const {
  const int wd =
      ::inotify_add_watch(m_inotify_fd, target.absolute.c_str(), k_watch_mask);
  if (wd >= 0) {
    m_watch_descriptors[wd] = target.relative;
  }

  // Recurse into existing real subdirectories. symlink_status(), not status(),
  // so a symlinked directory is left un-watched rather than descended into -
  // which is what keeps a symlink cycle from spinning here.
  std::error_code error;
  for (std::filesystem::directory_iterator it(target.absolute, error), end;
       !error && it != end; it.increment(error)) {
    std::error_code entry_error;
    const std::filesystem::file_status entry = it->symlink_status(entry_error);
    if (!entry_error && std::filesystem::is_directory(entry)) {
      add_watch({.absolute = it->path(),
                 .relative = target.relative / it->path().filename()});
    }
  }
}

void RealDirectoryTree::Impl::watch_loop() const {
  // alignas so each record can be read through at its natural alignment.
  alignas(struct inotify_event) std::array<char, k_event_buffer_bytes> buffer;

  while (true) {
    std::array<pollfd, 2> fds{
        {{.fd = m_inotify_fd, .events = POLLIN, .revents = 0},
         {.fd = m_stop_fd, .events = POLLIN, .revents = 0}}};
    if (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      return;  // Destructor signalled the eventfd.
    }
    if ((fds[0].revents & POLLIN) == 0) {
      continue;
    }

    // Ordered sets so repeated records for one path collapse and the lists come
    // out sorted, exactly as the Windows backend produces them.
    std::set<std::filesystem::path> files;
    std::set<std::filesystem::path> directories;
    bool overflowed = false;

    // Drain everything currently queued before notifying once, so a burst
    // becomes a single coalesced diff.
    while (true) {
      const ssize_t count = ::read(m_inotify_fd, buffer.data(), buffer.size());
      if (count <= 0) {
        if (count < 0 && errno == EINTR) {
          continue;
        }
        break;  // EAGAIN (drained) or an error we cannot act on.
      }

      std::size_t offset = 0;
      while (offset < static_cast<std::size_t>(count)) {
        const auto* event =
            reinterpret_cast<const inotify_event*>(buffer.data() + offset);
        offset += sizeof(inotify_event) + event->len;

        if ((event->mask & IN_Q_OVERFLOW) != 0) {
          overflowed = true;
          continue;
        }

        const auto watched = m_watch_descriptors.find(event->wd);
        if (watched == m_watch_descriptors.end()) {
          continue;  // A watch we have already forgotten.
        }

        if ((event->mask & IN_IGNORED) != 0) {
          // The watched directory itself went away; inotify has already removed
          // the watch, so drop our record of it.
          m_watch_descriptors.erase(watched);
          continue;
        }

        if (event->len == 0) {
          continue;  // An event on the directory itself, with no child named.
        }

        const std::filesystem::path& directory = watched->second;
        const std::filesystem::path relative =
            directory.empty() ? std::filesystem::path(event->name)
                              : directory / event->name;

        // Every record names something whose own identity moved.
        files.insert(relative);

        // Appearing, disappearing and renaming change the *directory's* child
        // list; a plain write or attribute change does not.
        if ((event->mask &
             (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) != 0) {
          directories.insert(directory);
        }

        // A directory that just appeared needs its own watch (inotify is not
        // recursive), together with anything already inside it.
        if ((event->mask & IN_ISDIR) != 0 &&
            (event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
          add_watch({.absolute = m_root / relative, .relative = relative});
        }
      }
    }

    DirectoryTreeDiff diff;
    diff.everything_dirty = overflowed;
    diff.entries_changed.assign(std::make_move_iterator(files.begin()),
                                std::make_move_iterator(files.end()));
    diff.child_lists_changed.assign(
        std::make_move_iterator(directories.begin()),
        std::make_move_iterator(directories.end()));

    // The interface promises a diff always says something, so stay silent when
    // a drain turned up nothing actionable (e.g. only bookkeeping events).
    if (diff.everything_dirty || !diff.entries_changed.empty() ||
        !diff.child_lists_changed.empty()) {
      notify_subscribers(diff);
    }
  }
}

void RealDirectoryTree::Impl::notify_subscribers(
    const DirectoryTreeDiff& diff) const {
  const std::vector<std::function<void(const DirectoryTreeDiff&)>> callbacks =
      [&] {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return std::vector<std::function<void(const DirectoryTreeDiff&)>>(
            m_subscribers.cbegin(), m_subscribers.cend());
      }();

  for (const std::function<void(const DirectoryTreeDiff&)>& cb : callbacks) {
    cb(diff);
  }
}

RealDirectoryTree::RealDirectoryTree(const std::filesystem::path& root)
    : m_impl(std::make_unique<Impl>(root)) {}

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
