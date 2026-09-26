// SPDX-License-Identifier: MIT
#include "virtualfilesystem.hpp"

#include "directorytree.hpp"
#include "processutil.hpp"
#include "tracer.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <fuse.h>
#include <fuse_opt.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace makebelieve {

namespace {

// The process the thread @a thread belongs to, read from `/proc`. FUSE names
// the calling thread rather than the program behind it, and it is the program
// a trace should group a request under, so its Tgid is what we want. A thread
// that has gone between making the request and this lookup leaves only its own
// id to go on.
std::uint32_t process_of_thread(std::uint32_t thread) {
  std::ifstream status("/proc/" + std::to_string(thread) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    constexpr std::string_view k_field = "Tgid:";
    if (!line.starts_with(k_field)) {
      continue;
    }
    const std::size_t start = line.find_first_not_of(" \t", k_field.size());
    if (start != std::string::npos) {
      return static_cast<std::uint32_t>(
          std::strtoul(line.c_str() + start, nullptr, 10));
    }
  }
  return thread;
}

// As above, remembering each answer: a thread belongs to the same process for
// as long as it lives, and reading `/proc` for every request would cost more
// than the request itself.
std::uint32_t process_of_caller(std::uint32_t thread) {
  static std::mutex mutex;
  static std::unordered_map<std::uint32_t, std::uint32_t> processes;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = processes.find(thread); it != processes.end()) {
      return it->second;
    }
  }
  // Outside the lock: two threads asking about the same caller at once only
  // means reading `/proc` twice.
  const std::uint32_t process = process_of_thread(thread);
  const std::lock_guard<std::mutex> lock(mutex);
  processes.insert_or_assign(thread, process);
  return process;
}

// The process a request came from, as a row's group in the trace.
TraceProcess calling_process(std::uint32_t thread) {
  // Only a process the trace has not seen needs a name looked up; the tracer
  // keeps the ones it has been given.
  const std::uint32_t process = process_of_caller(thread);
  return {.id = process,
          .name = g_tracer->knows_process(process)
                      ? std::string()
                      : ProcessUtil::name_of(process)};
}

// Turns a FUSE path (absolute, from the mount root) into the tree-relative
// path a DirectoryTree expects: "/" becomes the empty path (the root), and a
// leading slash is otherwise dropped so the result is not absolute.
std::filesystem::path to_tree_path(const char* fuse_path) {
  std::string_view view(fuse_path);
  if (!view.empty() && view.front() == '/') {
    view.remove_prefix(1);
  }
  return {view};
}

// Converts an EntryInfo mtime to the timespec a struct stat carries.
timespec to_timespec(std::chrono::file_clock::time_point time) {
  const auto system_time =
      std::chrono::clock_cast<std::chrono::system_clock>(time);
  const auto since_epoch = system_time.time_since_epoch();
  const auto seconds = std::chrono::floor<std::chrono::seconds>(since_epoch);
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
      since_epoch - seconds);
  return {
      .tv_sec = static_cast<time_t>(seconds.count()),
      .tv_nsec = static_cast<long>(nanoseconds.count()),
  };
}

// Throw if `mountpoint` already exists, otherwise attempt to create it +
// parents.
void create_mountpoint(const std::filesystem::path& mountpoint) {
  std::error_code error;
  if (std::filesystem::exists(mountpoint, error)) {
    throw std::filesystem::filesystem_error(
        "mountpoint already exists; refusing to reuse it", mountpoint,
        std::make_error_code(std::errc::file_exists));
  }
  std::filesystem::create_directories(mountpoint, error);
  if (error) {
    throw std::filesystem::filesystem_error("could not create mountpoint",
                                            mountpoint, error);
  }
}

// Removes a mountpoint this provider created, once the filesystem is unmounted.
// Not recursive: if the unmount silently failed we must not delete through the
// live mount. Don't throw as this is called from our destructors.
void remove_mountpoint(const std::filesystem::path& mountpoint) noexcept {
  std::error_code error;
  std::filesystem::remove(mountpoint, error);
}

constexpr std::string_view k_fake_write_contents{"\0", 1};

// How a change is announced. An addition dominates a modification when the two
// coalesce: a watcher that never heard of the new name only makes sense of an
// IN_CREATE.
enum class ChangeKind : std::uint8_t { modified, added };

using Changes = std::unordered_map<std::filesystem::path, ChangeKind>;

// Folds @a from into @a into, keeping the dominant kind for a repeated path.
void merge_changes(Changes& into, const Changes& from) {
  for (const auto& [path, kind] : from) {
    const auto [it, inserted] = into.try_emplace(path, kind);
    if (!inserted && kind == ChangeKind::added) {
      it->second = ChangeKind::added;
    }
  }
}

}  // namespace

// libfuse provider over a DirectoryTree.
class VirtualFileSystem::Impl {
  // One row per open being served at once, so a reader's whole request - the
  // build it waits for included - reads as one worker rather than as whichever
  // FUSE thread happened to take it.
  mutable TraceLanePool m_reader_lanes{"reader"};

  const DirectoryTree& m_tree;
  std::filesystem::path m_mountpoint;

  // Where notifier passes run.
  exec::static_thread_pool::scheduler m_scheduler;
  fuse_args m_args{};
  struct fuse* m_fuse = nullptr;
  std::thread m_loop;

  // Entries the kernel has seen through us, by lookup or in a listing, each
  // mapped to whether it is a directory. A watch on a directory covers children
  // nothing has looked up, so this does not bound who can be told about an
  // ordinary change. It is what an "everything changed" pass falls back on,
  // since walking our own mount from the notifier is not an option, and it is
  // the only record of what a removed entry was once the tree has let it go.
  // It only grows while the mount lives (minus what announce_removal()
  // forgets), which bounds it by what has actually been touched rather than
  // by the size of the tree.
  std::mutex m_known_mutex;
  std::unordered_map<std::filesystem::path, bool> m_known;

  // What the notifier is pretending about one path while it creates or
  // removes that path through the mount (see announce_creation() and
  // announce_removal()). Only the notifier's own requests see the pretence;
  // everyone else is always told what the tree holds.
  enum class Pretence : std::uint8_t {
    none,
    absent,
    present_file,
    present_directory
  };
  std::mutex m_pretence_mutex;
  std::filesystem::path m_pretence_path;
  Pretence m_pretence = Pretence::none;

  // The queue on_tree_changed hands the notifier, and whether a pass of it is
  // under way. m_pending coalesces: a path repeated across diffs is announced
  // once per pass, and m_everything_dirty supersedes the lot.
  std::mutex m_notify_mutex;
  Changes m_pending;
  bool m_everything_dirty = false;
  bool m_stopping = false;
  bool m_notifying = false;

  // gettid() of the thread running a notifier pass, and 0 between passes,
  // which is never a valid tid.
  std::atomic<pid_t> m_notifier_tid{0};

  // The notifier pass under way, if any, joined on destruction.
  stdexec::counting_scope m_notifications;

  // Declared last so it is destroyed first, stopping notifications before the
  // state they touch goes away.
  std::optional<Subscription> m_subscription;

 public:
  // Selects the constructor that brings the object into existence without
  // starting anything, so that the one below can delegate to it.
  struct Unstarted {};

  Impl(const DirectoryTree& tree,
       const std::filesystem::path& mountpoint,
       exec::static_thread_pool::scheduler scheduler)
      : Impl(tree, mountpoint, scheduler, Unstarted{}) {
    m_args = FUSE_ARGS_INIT(0, nullptr);
    if (fuse_opt_add_arg(&m_args, "makebelieve") != 0) {
      const int error = errno != 0 ? errno : EIO;
      throw std::system_error(error, std::system_category(),
                              "fuse_opt_add_arg failed");
    }

    const fuse_operations operations = {
        .getattr = trampoline<&Impl::op_getattr>,
        .mkdir = trampoline<&Impl::op_mkdir>,
        .unlink = trampoline<&Impl::op_unlink>,
        .rmdir = trampoline<&Impl::op_rmdir>,
        .open = trampoline<&Impl::op_open>,
        .read = trampoline<&Impl::op_read>,
        .write = trampoline<&Impl::op_write>,
        .readdir = trampoline<&Impl::op_readdir>,
        .init = &Impl::op_init,
        .create = trampoline<&Impl::op_create>,
    };

    m_fuse = fuse_new(&m_args, &operations, sizeof(operations), this);
    if (m_fuse == nullptr) {
      const int error = errno != 0 ? errno : EIO;
      throw std::system_error(error, std::system_category(), "fuse_new failed");
    }

    if (fuse_mount(m_fuse, m_mountpoint.c_str()) != 0) {
      const int error = errno != 0 ? errno : EIO;
      throw std::system_error(error, std::system_category(),
                              "fuse_mount failed");
    }

    // Multithreaded, so an open blocked on a build (see op_open) does not hold
    // up other requests. Every callback here is safe to run concurrently.
    m_loop = std::thread([this]() {
      fuse_loop_config config{.clone_fd = 0, .max_idle_threads = 10};
      fuse_loop_mt(m_fuse, &config);
    });

    // The subscription is taken last so no change can arrive before there is a
    // mount to poke.
    m_subscription.emplace(m_tree.subscribe_to_changes(
        [this](const DirectoryTreeDiff& diff) { on_tree_changed(diff); }));
  }

  ~Impl() {
    m_subscription.reset();

    // A pass under way sees m_stopping and ends after the change it is on.
    {
      const std::lock_guard<std::mutex> lock(m_notify_mutex);
      m_stopping = true;
    }
    stdexec::sync_wait(m_notifications.join());

    if (m_fuse != nullptr) {
      fuse_exit(m_fuse);
      fuse_unmount(m_fuse);
      if (m_loop.joinable()) {
        m_loop.join();
      }
      fuse_destroy(m_fuse);
    }
    fuse_opt_free_args(&m_args);

    // The mountpoint is an ordinary empty directory again now that the
    // filesystem is unmounted, so it is safe to remove. Reaching the
    // destructor at all means the constructor below created it, so this only
    // ever removes what this object made.
    remove_mountpoint(m_mountpoint);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

 private:
  // Acquires the mountpoint and nothing else. Called from our other
  // constructor so if we throw from it, our destructor will be called and
  // we can cleanup.
  Impl(const DirectoryTree& tree,
       const std::filesystem::path& mountpoint,
       exec::static_thread_pool::scheduler scheduler,
       Unstarted)
      // Absolute, because the notifier reopens paths beneath it for as long
      // as the mount lives, by which time the process's working directory may
      // have moved on from whatever made a relative path meaningful.
      : m_tree(tree),
        m_mountpoint(std::filesystem::absolute(mountpoint)),
        m_scheduler(scheduler) {
    create_mountpoint(m_mountpoint);
  }

  // Bridges a fuse_operations C callback to a member function.
  template <auto MemFn>
  struct Bridge;

  template <typename... Args, int (Impl::*MemFn)(Args...)>
  struct Bridge<MemFn> {
    static int call(Args... args) {
      try {
        auto* self = static_cast<Impl*>(fuse_get_context()->private_data);
        return (self->*MemFn)(args...);
      } catch (const std::bad_alloc&) {
        return -ENOMEM;
      } catch (const std::system_error& error) {
        // Everything this filesystem talks to deals in errno values, so a
        // code that escaped as an exception is handed back the same way
        // op_read hands back the ones the tree returns.
        const int code = error.code().value();
        return code > 0 ? -code : -EIO;
      } catch (...) {
        return -EIO;
      }
    }
  };

  template <auto MemFn>
  static constexpr auto trampoline = Bridge<MemFn>::call;

  // Whether the request being serviced originates from a notifier pass, the
  // only writer this filesystem accepts.
  [[nodiscard]] bool is_self_request() const {
    // `m_notifier_tid` is 0 between passes.
    const pid_t writer = m_notifier_tid.load(std::memory_order_relaxed);
    return writer != 0 && fuse_get_context()->pid == writer;
  }

  // Readers go through the kernel's page cache (see op_open), so what keeps
  // them from being served stale pages is the kernel re-asking for attributes
  // on every access - the zero timeouts - and dropping cached pages whenever
  // those attributes show the file moved on. AUTO_INVAL_DATA is what makes it
  // drop them for a change of mtime as well as of size, which is the only
  // sign of a same-size rewrite; libfuse normally enables it, but a stale
  // page is silent corruption, so it is asked for rather than assumed.
  static void* op_init(fuse_conn_info* conn, fuse_config* config) {
    config->entry_timeout = 0;
    config->attr_timeout = 0;
    config->negative_timeout = 0;
    // An announced removal really unlinks the entry, even while something
    // holds it open. Without this, libfuse would try to hide an open file by
    // renaming it instead, which this filesystem cannot do.
    config->hard_remove = 1;
    if ((conn->capable & FUSE_CAP_AUTO_INVAL_DATA) != 0) {
      conn->want |= FUSE_CAP_AUTO_INVAL_DATA;
      conn->want &= ~FUSE_CAP_EXPLICIT_INVAL_DATA;
    }
    return fuse_get_context()->private_data;
  }

  // Reports an entry's metadata. Everything is read-only: directories 0555,
  // files 0444.
  int op_getattr(const char* path, struct stat* out, fuse_file_info* /*info*/) {
    const std::filesystem::path relative = to_tree_path(path);
    std::memset(out, 0, sizeof(*out));
    if (is_self_request()) {
      switch (pretence_for(relative)) {
        case Pretence::none:
          break;
        case Pretence::absent:
          return -ENOENT;
        case Pretence::present_file:
          out->st_mode = S_IFREG | 0444;
          out->st_nlink = 1;
          return 0;
        case Pretence::present_directory:
          out->st_mode = S_IFDIR | 0555;
          out->st_nlink = 2;
          return 0;
      }
    }

    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(relative);
    if (!status.has_value()) {
      return -ENOENT;
    }

    // Every watch starts with a lookup, and entry caching is off, so every such
    // lookup lands here.
    remember(relative, std::holds_alternative<DirectoryInfo>(*status));
    if (const auto* file = std::get_if<FileInfo>(&*status)) {
      out->st_mode = S_IFREG | 0444;
      out->st_nlink = 1;
      out->st_size = static_cast<off_t>(file->size);
      const timespec time = to_timespec(file->mtime);
      out->st_atim = time;
      out->st_mtim = time;
      out->st_ctim = time;
      return 0;
    }

    const auto& directory = std::get<DirectoryInfo>(*status);
    out->st_mode = S_IFDIR | 0555;
    out->st_nlink = 2;
    const timespec time = to_timespec(directory.mtime);
    out->st_atim = time;
    out->st_mtim = time;
    out->st_ctim = time;
    return 0;
  }

  // Lists a directory's children. Attribute caching is off, so the per-entry
  // stat is left to op_getattr rather than primed here.
  int op_readdir(const char* path,
                 void* buffer,
                 fuse_fill_dir_t filler,
                 off_t /*offset*/,
                 fuse_file_info* /*info*/,
                 fuse_readdir_flags /*flags*/) {
    const std::filesystem::path directory = to_tree_path(path);
    const std::expected<std::vector<TreeEntry>, std::error_code> entries =
        m_tree.ls(directory);
    if (!entries.has_value()) {
      return -ENOENT;
    }

    // We need to supply the standard self/parent entries.
    filler(buffer, ".", nullptr, 0, fuse_fill_dir_flags{});
    filler(buffer, "..", nullptr, 0, fuse_fill_dir_flags{});
    for (const TreeEntry& entry : *entries) {
      // TreeEntry::name is the leaf name only. A listed entry is one the
      // kernel's caller now knows about, so its removal is worth announcing
      // even if nothing ever looks it up.
      remember(directory / entry.name,
               std::holds_alternative<DirectoryInfo>(entry.info));
      filler(buffer, entry.name.c_str(), nullptr, 0, fuse_fill_dir_flags{});
    }
    return 0;
  }

  // Opens a file. Readers get the kernel's page cache rather than direct_io,
  // because direct_io rules out shared memory mappings: the kernel only allows
  // them on a direct_io file when the filesystem negotiates
  // FUSE_DIRECT_IO_ALLOW_MMAP, which libfuse cannot request before 3.16 and
  // Ubuntu 24.04 ships 3.14. Compilers, linkers and indexers map their inputs,
  // so that is not a corner worth giving up.
  //
  // A read open blocks in the tree's open() until the file is final; a stat
  // never comes here. With attribute caching off, the kernel re-reads the size
  // before a read or fstat, so it sees the settled one.
  int op_open(const char* path, fuse_file_info* info) {
    const std::filesystem::path relative = to_tree_path(path);
    // FUSE names the calling thread, which is the process itself for a
    // single-threaded reader.
    const auto caller = static_cast<std::uint32_t>(fuse_get_context()->pid);
    MB_TRACE_POOL_SCOPE(m_reader_lanes, calling_process(caller), "vfs",
                        std::tie("open", relative), "caller", caller);
    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(relative);
    if (!status.has_value()) {
      return -ENOENT;
    }
    if (std::holds_alternative<DirectoryInfo>(*status)) {
      return -EISDIR;
    }
    if ((info->flags & O_ACCMODE) != O_RDONLY) {
      // 0444 is advertised for every file, but that is cosmetic without
      // default_permissions. This check is what actually keeps the projection
      // read-only, and what carves out the notifier's own write.
      if (!is_self_request()) {
        return -EACCES;
      }
      // The notifier's write must not touch the page cache. A buffered write
      // would copy its placeholder byte into the cached page before op_write
      // ever sees it, and a reader mapping the file at that moment would see
      // that byte in place of the real content.
      info->direct_io = 1;
      return 0;
    }

    if (const std::expected<FileInfo, std::error_code> opened =
            m_tree.open(relative);
        !opened.has_value()) {
      const int code = opened.error().value();
      return code > 0 ? -code : -EIO;
    }
    return 0;
  }

  // Hydrates up to `size` bytes at `offset` from the tree. A result shorter
  // than `size` is end of file.
  int op_read(const char* path,
              char* buffer,
              size_t size,
              off_t offset,
              fuse_file_info* /*info*/) {
    const std::expected<std::string, std::error_code> bytes =
        m_tree.read(to_tree_path(path), static_cast<Offset>(offset), size);
    if (!bytes.has_value()) {
      // The tree's error codes are errno values; hand them straight back as
      // the negative errno FUSE expects, falling back to EIO for anything odd.
      const int code = bytes.error().value();
      return code > 0 ? -code : -EIO;
    }

    const size_t count = std::min(size, bytes->size());
    // A FUSE read buffer holds a counted byte range, not a string - the
    // length goes back as the return value, so there is nothing to terminate.
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    std::memcpy(buffer, bytes->data(), count);
    return static_cast<int>(count);
  }

  // Swallows the notifier's own write and rejects everyone else's.
  int op_write(const char* /*path*/,
               [[maybe_unused]] const char* buffer,
               size_t size,
               off_t /*offset*/,
               fuse_file_info* /*info*/) {
    if (!is_self_request()) {
      return -EACCES;
    }
    // Our self writes are only to raise fsnotify events as this is not
    // directly supported in FUSE.
    assert(std::string_view(buffer, size) == k_fake_write_contents);
    return static_cast<int>(size);
  }

  // The four ways into the tree's namespace, which, like op_write, exist only
  // for the notifier. Its creates and removals are how the kernel is made to
  // raise IN_CREATE and IN_DELETE (see announce_creation() and
  // announce_removal()); each ends the pretence that let the kernel go ahead,
  // so the lookup libfuse makes straight afterwards sees the tree as it is.
  // Anyone else is refused, which is what keeps the projection read-only.
  int op_create(const char* path, mode_t /*mode*/, fuse_file_info* info) {
    if (!end_pretence(path, Pretence::absent)) {
      return -EACCES;
    }
    // Nothing is ever written through this handle, but direct_io keeps it
    // from touching the page cache all the same (see op_open).
    info->direct_io = 1;
    return 0;
  }

  int op_mkdir(const char* path, mode_t /*mode*/) {
    return end_pretence(path, Pretence::absent) ? 0 : -EACCES;
  }

  int op_unlink(const char* path) {
    return end_pretence(path, Pretence::present_file) ? 0 : -EACCES;
  }

  int op_rmdir(const char* path) {
    return end_pretence(path, Pretence::present_directory) ? 0 : -EACCES;
  }

  // What the notifier's own requests should be told about @a path.
  [[nodiscard]] Pretence pretence_for(const std::filesystem::path& path) {
    const std::lock_guard<std::mutex> lock(m_pretence_mutex);
    return path == m_pretence_path ? m_pretence : Pretence::none;
  }

  // Pretends, to the notifier alone, that @a path is in state @a pretence.
  void pretend(const std::filesystem::path& path, Pretence pretence) {
    const std::lock_guard<std::mutex> lock(m_pretence_mutex);
    m_pretence_path = path;
    m_pretence = pretence;
  }

  // Ends the pretence if it is the notifier asking about the path it is
  // pretending @a expected for, and reports whether it was.
  bool end_pretence(const char* fuse_path, Pretence expected) {
    if (!is_self_request()) {
      return false;
    }
    const std::lock_guard<std::mutex> lock(m_pretence_mutex);
    if (m_pretence != expected || m_pretence_path != to_tree_path(fuse_path)) {
      return false;
    }
    m_pretence = Pretence::none;
    return true;
  }

  // Records an entry the kernel has seen through us.
  void remember(const std::filesystem::path& path, bool is_directory) {
    const std::lock_guard<std::mutex> lock(m_known_mutex);
    m_known.insert_or_assign(path, is_directory);
  }

  // Queues a tree change for the notifier, starting a pass if none is under
  // way.
  //
  // Runs on whichever thread the tree notifies from, so it does no I/O. A
  // poke writes into our own mount and blocks until the FUSE loop has
  // serviced it, which is not something to make the tree's watcher wait on:
  // stall that thread and its own event queue is what overflows.
  void on_tree_changed(const DirectoryTreeDiff& diff) {
    const std::lock_guard<std::mutex> lock(m_notify_mutex);
    if (m_stopping) {
      return;
    }
    if (diff.everything_dirty) {
      m_everything_dirty = true;
    } else {
      // An entry whose parent's child list changed in the same diff was added
      // or removed rather than rewritten; which of the two is settled when it
      // is announced, by whether it still exists. That is all
      // child_lists_changed is needed for: a watcher on the parent learns
      // about a new or missing child from the child's own event.
      const std::unordered_set<std::filesystem::path> parents(
          diff.child_lists_changed.begin(), diff.child_lists_changed.end());
      Changes changes;
      for (const std::filesystem::path& path : diff.entries_changed) {
        changes.emplace(path, parents.contains(path.parent_path())
                                  ? ChangeKind::added
                                  : ChangeKind::modified);
      }
      merge_changes(m_pending, changes);
    }
    start_notifying();
  }

  // Starts a pass announcing the queued changes, unless one is under way or we
  // are stopping. A pass that cannot be started is retried by the next change.
  // @pre m_notify_mutex is held.
  void start_notifying() noexcept {
    if (m_notifying || m_stopping) {
      return;
    }
    m_notifying = true;
    try {
      stdexec::spawn(stdexec::schedule(m_scheduler) |
                         stdexec::then([this]() noexcept { notify_pending(); }),
                     m_notifications.get_token());
    } catch (...) {
      m_notifying = false;
    }
  }

  // One pass of the notifier: pokes each affected path in turn until nothing
  // is queued, then stands down.
  void notify_pending() noexcept {
    MB_TRACE_SCOPE("vfs", "notify");
    // Claimed for the whole pass rather than per write: only one pass runs at
    // a time, and this thread runs nothing else meanwhile, so a request
    // carrying this tid is ours by construction (see is_self_request).
    m_notifier_tid.store(::gettid(), std::memory_order_relaxed);

    std::unique_lock<std::mutex> lock(m_notify_mutex);
    // Best-effort: a change dropped for want of memory goes unannounced, as
    // one a watcher's own queue overflows on does.
    try {
      while (!m_stopping && (m_everything_dirty || !m_pending.empty())) {
        Changes batch;
        if (m_everything_dirty) {
          // The tree lost track of what changed, so everything we have handed
          // out could be stale. Announce the paths we know the kernel has seen
          // rather than walking the mount: an enumeration of our own
          // mountpoint would re-enter the FUSE callbacks from here and wait on
          // a loop that is waiting on us.
          m_everything_dirty = false;
          m_pending.clear();
          const std::lock_guard<std::mutex> known_lock(m_known_mutex);
          for (const auto& [path, is_directory] : m_known) {
            batch.emplace(path, ChangeKind::modified);
          }
        } else {
          batch = std::exchange(m_pending, {});
        }

        // Unlocked for the announcements themselves, so a change arriving
        // mid-batch simply queues up for the next round.
        lock.unlock();
        for (const auto& [path, kind] : batch) {
          announce(path, kind);
        }
        lock.lock();
      }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      if (!lock.owns_lock()) {
        lock.lock();
      }
    }

    // Released before the next pass can start, which only this lets happen.
    m_notifier_tid.store(0, std::memory_order_relaxed);
    m_notifying = false;
  }

  // Makes the kernel raise the inotify event that describes one change. FUSE
  // has no way to announce a change directly: the kernel raises an event only
  // for an operation that really passes through the VFS. So the notifier
  // performs the operation itself, through the mount, and the callbacks above
  // let exactly that request through.
  //
  // Every changed path is announced, whether or not the kernel has seen it: a
  // watch on its directory reports changes to children nothing has looked up.
  void announce(const std::filesystem::path& path, ChangeKind kind) {
    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(path);
    if (!status.has_value()) {
      announce_removal(path);
    } else if (kind == ChangeKind::added) {
      announce_creation(path, std::holds_alternative<DirectoryInfo>(*status));
    } else if (std::holds_alternative<FileInfo>(*status)) {
      poke(path);
    }
    // A directory that changed without being added has nothing a watcher can
    // be told: its own events are its children's.
  }

  // Raises IN_CREATE on the parent by creating @a path through the mount. The
  // tree already holds it, so the notifier's lookup is told it is absent;
  // otherwise the kernel would refuse to create what already exists.
  void announce_creation(const std::filesystem::path& path, bool is_directory) {
    const std::filesystem::path mounted = m_mountpoint / path;
    pretend(path, Pretence::absent);
    // Best effort, like poke(): if the kernel refuses, a watcher simply learns
    // of the entry the next time it looks.
    if (is_directory) {
      static_cast<void>(::mkdir(mounted.c_str(), 0555));
    } else {
      static_cast<void>(::mknod(mounted.c_str(), S_IFREG | 0444, 0));
    }
    pretend(path, Pretence::none);
    remember(path, is_directory);
  }

  // Raises IN_DELETE on the parent by removing @a path through the mount. The
  // tree no longer holds it, so the notifier's lookup is told it is still
  // there, as whatever the kernel last saw it as.
  void announce_removal(const std::filesystem::path& path) {
    std::optional<bool> is_directory;
    {
      // Forgotten either way, so a path that comes back is re-learned by the
      // lookup that finds it.
      const std::lock_guard<std::mutex> lock(m_known_mutex);
      if (const auto it = m_known.find(path); it != m_known.end()) {
        is_directory = it->second;
        m_known.erase(it);
      }
    }
    // Something the kernel never saw has no dentry and nothing watching it by
    // name, and something whose parent went too is covered by that parent's
    // own removal.
    if (!is_directory.has_value() ||
        !m_tree.status(path.parent_path()).has_value()) {
      return;
    }

    const std::filesystem::path mounted = m_mountpoint / path;
    if (*is_directory) {
      pretend(path, Pretence::present_directory);
      static_cast<void>(::rmdir(mounted.c_str()));
    } else {
      pretend(path, Pretence::present_file);
      static_cast<void>(::unlink(mounted.c_str()));
    }
    pretend(path, Pretence::none);
  }

  // Makes the kernel raise IN_MODIFY for one changed file, by opening it
  // through the mount and writing a byte that op_write throws away.
  void poke(const std::filesystem::path& path) {
    const std::filesystem::path mounted = m_mountpoint / path;
    const int fd = ::open(mounted.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
      return;
    }
    // A (void) cast does not silence warn_unused_result on GCC, so the result
    // is bound and discarded the way handle_signal does it. Nothing here could
    // act on a failed write anyway: the poke is best effort.
    const ssize_t written =
        ::write(fd, k_fake_write_contents.data(), k_fake_write_contents.size());
    static_cast<void>(written);
    ::close(fd);
  }
};

VirtualFileSystem::VirtualFileSystem(
    const DirectoryTree& tree,
    const std::filesystem::path& mountpoint,
    exec::static_thread_pool::scheduler scheduler)
    : m_impl(std::make_unique<Impl>(tree, mountpoint, scheduler)) {}

VirtualFileSystem::~VirtualFileSystem() = default;

}  // namespace makebelieve
