// SPDX-License-Identifier: MIT
#include "virtualfilesystem.hpp"

#include "directorytree.hpp"
#include "iocontext.hpp"
#include "processinfo.hpp"
#include "tracer.hpp"

#include <fuse_lowlevel.h>
#include <fuse_opt.h>
#include <exec/task.hpp>
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
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
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

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace makebelieve {

namespace {

// The errno a tree error reaches FUSE as. Only the errno categories carry
// errno values; any other - a command's exit status, say - is a failure this
// filesystem has no errno for, and reads as an I/O error rather than as
// whatever errno shares its number.
int to_errno(const std::error_code& error) {
  if ((error.category() == std::generic_category() ||
       error.category() == std::system_category()) &&
      error.value() > 0) {
    return error.value();
  }
  return EIO;
}

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
                      : ProcessInfo::name_of(process)};
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
  fuse_session* m_fuse = nullptr;
  stdexec::inplace_stop_source m_request_stop;
  stdexec::counting_scope m_requests;
  IoContext m_file_io;
  struct FileHandle {
    std::filesystem::path path;
    std::vector<std::string> names;
  };
  // Unmount may discard kernel handles without sending release callbacks.
  std::mutex m_handle_mutex;
  std::uint64_t m_last_handle = 0;
  std::unordered_map<std::uint64_t, std::unique_ptr<FileHandle>> m_handles;
  std::mutex m_inode_mutex;
  std::unordered_map<std::filesystem::path, fuse_ino_t> m_ids{
      {{}, FUSE_ROOT_ID}};
  struct Node {
    std::filesystem::path path;
    std::uint64_t lookups = 0;
  };
  std::unordered_map<fuse_ino_t, Node> m_paths{
      {FUSE_ROOT_ID, Node{.path = {}, .lookups = 1}}};
  fuse_ino_t m_next_inode = FUSE_ROOT_ID;

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

    const fuse_lowlevel_ops operations = {
        .init = &Impl::op_init,
        .lookup = bridge<&Impl::lookup>,
        .forget = &Impl::op_forget,
        .getattr = bridge<&Impl::getattr>,
        .mknod = bridge<&Impl::mknod>,
        .mkdir = bridge<&Impl::mkdir>,
        .unlink = bridge<&Impl::unlink>,
        .rmdir = bridge<&Impl::rmdir>,
        .open = bridge<&Impl::open>,
        .read = bridge<&Impl::read>,
        .write = bridge<&Impl::write>,
        .release = bridge<&Impl::release>,
        .opendir = bridge<&Impl::opendir>,
        .readdir = bridge<&Impl::readdir>,
        .releasedir = bridge<&Impl::releasedir>,
        .create = bridge<&Impl::create>,
    };
    m_fuse = fuse_session_new(&m_args, &operations, sizeof(operations), this);
    if (!m_fuse) {
      throw std::system_error(errno ? errno : EIO, std::system_category(),
                              "fuse_session_new");
    }
    if (fuse_session_mount(m_fuse, m_mountpoint.c_str()) != 0) {
      throw std::system_error(errno ? errno : EIO, std::system_category(),
                              "fuse_session_mount");
    }
    // A single dispatcher suffices: waiting requests retain their reply handle,
    // not this thread. Completions can reply concurrently from coroutine
    // workers.
    m_loop = std::thread([this] { fuse_session_loop(m_fuse); });

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
      m_request_stop.request_stop();
      stdexec::sync_wait(m_requests.join());
      fuse_session_exit(m_fuse);
      fuse_session_unmount(m_fuse);
      if (m_loop.joinable()) {
        m_loop.join();
      }
      fuse_session_destroy(m_fuse);
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

  template <auto Member>
  struct Bridge;
  template <class... Args, void (Impl::*Member)(fuse_req_t, Args...)>
  struct Bridge<Member> {
    static void call(fuse_req_t req, Args... args) noexcept {
      try {
        (static_cast<Impl*>(fuse_req_userdata(req))->*Member)(req, args...);
      } catch (const std::bad_alloc&) {
        fuse_reply_err(req, ENOMEM);
      } catch (const std::system_error& e) {
        fuse_reply_err(req, to_errno(e.code()));
      } catch (...) {
        fuse_reply_err(req, EIO);
      }
    }
  };
  template <auto Member>
  static constexpr auto bridge = Bridge<Member>::call;

  bool is_self_request(fuse_req_t req) const {
    const auto writer = m_notifier_tid.load(std::memory_order_relaxed);
    return writer && fuse_req_ctx(req)->pid == writer;
  }
  static void op_init(void*, fuse_conn_info* conn) {
    if (conn->capable & FUSE_CAP_AUTO_INVAL_DATA) {
      conn->want |= FUSE_CAP_AUTO_INVAL_DATA;
      conn->want &= ~FUSE_CAP_EXPLICIT_INVAL_DATA;
    }
  }
  // Lookup references belong to the kernel. Open handles own their path
  // separately, so forgetting a lookup never invalidates an existing handle.
  fuse_ino_t inode(const std::filesystem::path& name) {
    const std::lock_guard lock(m_inode_mutex);
    auto [it, inserted] = m_ids.try_emplace(name, m_next_inode + 1);
    if (inserted) {
      try {
        m_paths.emplace(++m_next_inode, Node{.path = name});
      } catch (...) {
        m_ids.erase(it);
        throw;
      }
    }
    ++m_paths.at(it->second).lookups;
    return it->second;
  }
  std::filesystem::path path(fuse_ino_t ino) {
    const std::lock_guard lock(m_inode_mutex);
    return m_paths.at(ino).path;
  }
  // Inode numbers and lookup counts share a platform integer type.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void forget_inode(fuse_ino_t ino, std::uint64_t count) noexcept {
    const std::lock_guard lock(m_inode_mutex);
    const auto found = m_paths.find(ino);
    if (found == m_paths.end() || ino == FUSE_ROOT_ID) {
      return;
    }
    found->second.lookups -= std::min(count, found->second.lookups);
    if (found->second.lookups == 0) {
      m_ids.erase(found->second.path);
      m_paths.erase(found);
    }
  }
  // Forget has no error reply. Bypass the exception-to-errno bridge, and keep
  // reference cleanup non-throwing, including after a failed entry reply.
  static void op_forget(fuse_req_t req,
                        fuse_ino_t ino,
                        std::uint64_t count) noexcept {
    static_cast<Impl*>(fuse_req_userdata(req))->forget_inode(ino, count);
    fuse_reply_none(req);
  }
  std::uint64_t retain(std::unique_ptr<FileHandle> handle) {
    const std::lock_guard lock(m_handle_mutex);
    const auto result = ++m_last_handle;
    m_handles.emplace(result, std::move(handle));
    return result;
  }
  FileHandle& file_handle(std::uint64_t handle) {
    const std::lock_guard lock(m_handle_mutex);
    return *m_handles.at(handle);
  }
  // May run after fuse_reply_open has consumed the request. Never unwind into
  // the bridge and cause a second reply if cleanup cannot take its lock.
  void drop(std::uint64_t handle) noexcept {
    const std::lock_guard lock(m_handle_mutex);
    m_handles.erase(handle);
  }
  void release(fuse_req_t req, fuse_ino_t, fuse_file_info* info) {
    drop(info->fh);
    fuse_reply_err(req, 0);
  }
  int attributes(fuse_req_t req,
                 const std::filesystem::path& relative,
                 struct stat* out) {
    std::memset(out, 0, sizeof(*out));
    const Pretence pretence =
        is_self_request(req) ? pretence_for(relative) : Pretence::none;
    if (pretence == Pretence::absent) {
      return -ENOENT;
    }

    const std::expected<EntryInfo, std::error_code> status =
        m_tree.status(relative);
    if (!status.has_value()) {
      // Made up only while the path really is gone. One recreated since its
      // removal was queued shares the kernel's inode with it, so made-up
      // attributes - a size of 0 - would be what everyone else read too.
      switch (pretence) {
        case Pretence::present_file:
          out->st_mode = S_IFREG | 0444;
          out->st_nlink = 1;
          return 0;
        case Pretence::present_directory:
          out->st_mode = S_IFDIR | 0555;
          out->st_nlink = 2;
          return 0;
        default:
          return -ENOENT;
      }
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

  void entry(fuse_req_t req,
             const std::filesystem::path& name,
             fuse_file_info* fi = nullptr) {
    fuse_entry_param value{};
    const int error = attributes(req, name, &value.attr);
    if (error) {
      fuse_reply_err(req, -error);
      return;
    }
    value.ino = inode(name);
    value.generation = 1;
    value.attr.st_ino = value.ino;
    const int replied =
        fi ? fuse_reply_create(req, &value, fi) : fuse_reply_entry(req, &value);
    if (replied != 0) {
      forget_inode(value.ino, 1);
    }
  }
  void lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    entry(req, path(parent) / name);
  }
  void getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* handle) {
    struct stat value {};
    const auto name =
        handle && handle->fh ? file_handle(handle->fh).path : path(ino);
    const int error = attributes(req, name, &value);
    value.st_ino = ino;
    if (error) {
      fuse_reply_err(req, -error);
    } else {
      fuse_reply_attr(req, &value, 0);
    }
  }

  // The kernel request survives the dispatch callback. Its owner unregisters
  // interrupts before replying, and guarantees a reply even on cancellation or
  // an exception in the coroutine. No pointers to callback arguments escape.
  struct Request {
    fuse_req_t req;
    stdexec::inplace_stop_source stop;
    // Keep the first cancellation cause: shutdown and a kernel interrupt can
    // race, but only the latter should make the caller retry with EINTR.
    std::atomic<int> cancellation{0};
    void cancel(int error) noexcept {
      int none = 0;
      cancellation.compare_exchange_strong(none, error);
      stop.request_stop();
    }
    struct Stop {
      Request* self;
      void operator()() const noexcept { self->cancel(ECANCELED); }
    };
    stdexec::inplace_stop_callback<Stop> shutdown;
    Request(fuse_req_t request, stdexec::inplace_stop_token token)
        : req(request), shutdown(token, Stop{this}) {
      fuse_req_interrupt_func(
          req,
          [](fuse_req_t, void* ptr) {
            static_cast<Request*>(ptr)->cancel(EINTR);
          },
          this);
    }
    ~Request() { reply_error(ECANCELED); }
    void reply_error(int error) {
      if (!req) {
        return;
      }
      // Unregister the interrupt callback before inspecting its final cause.
      // Also handles a stopped sender, whose coroutine never reaches a reply.
      const auto request = take();
      if (error == ECANCELED && cancellation.load() == EINTR) {
        error = EINTR;
      }
      fuse_reply_err(request, error);
    }
    fuse_req_t take() {
      fuse_req_interrupt_func(req, nullptr, nullptr);
      return std::exchange(req, nullptr);
    }
  };
  exec::task<void> open_file(std::shared_ptr<Request> request,
                             std::filesystem::path name,
                             fuse_file_info info,
                             std::uint32_t caller) {
    TraceAsyncScope trace;
    if (g_tracer) {
      trace.open(m_reader_lanes, calling_process(caller), name);
    }
    try {
      auto handle = std::make_unique<FileHandle>(FileHandle{.path = name});
      auto opened = co_await stdexec::write_env(
          m_tree.open(name, OpenContext{.requester_pid = caller}),
          stdexec::prop{stdexec::get_stop_token, request->stop.get_token()});
      if (!opened) {
        request->reply_error(to_errno(opened.error()));
      } else {
        info.fh = retain(std::move(handle));
        if (fuse_reply_open(request->take(), &info) != 0) {
          drop(info.fh);
        }
      }
    } catch (...) {
      request->reply_error(EIO);
    }
  }
  void open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
      if (!is_self_request(req)) {
        fuse_reply_err(req, EACCES);
        return;
      }
      fi->direct_io = 1;
      fuse_reply_open(req, fi);
      return;
    }
    auto name = path(ino);
    auto caller = static_cast<std::uint32_t>(fuse_req_ctx(req)->pid);
    auto request = std::make_shared<Request>(req, m_request_stop.get_token());
    // Ownership has passed to Request: errors below must not trigger a second
    // reply.
    try {
      stdexec::spawn(
          stdexec::starts_on(m_file_io.get_scheduler(),
                             open_file(request, std::move(name), *fi, caller)) |
              stdexec::upon_error([](const std::exception_ptr&) noexcept {}),
          m_requests.get_token());
    } catch (...) {
      request.reset();  // The final request owner supplies a failure reply.
    }
  }
  exec::task<void> read_file(std::shared_ptr<Request> request,
                             std::filesystem::path name,
                             size_t size,
                             off_t offset) {
    try {
      auto bytes = co_await stdexec::write_env(
          m_tree.read(name, offset, size),
          stdexec::prop{stdexec::get_stop_token, request->stop.get_token()});
      if (!bytes) {
        request->reply_error(to_errno(bytes.error()));
      } else {
        fuse_reply_buf(request->take(), bytes->data(),
                       std::min(size, bytes->size()));
      }
    } catch (...) {
      request->reply_error(EIO);
    }
  }
  // NOLINTBEGIN(bugprone-easily-swappable-parameters): libfuse callback
  // signature.
  void read(fuse_req_t req,
            fuse_ino_t ino,
            size_t size,
            off_t offset,
            fuse_file_info* info) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    auto name = info->fh ? file_handle(info->fh).path : path(ino);
    auto request = std::make_shared<Request>(req, m_request_stop.get_token());
    try {
      stdexec::spawn(
          stdexec::starts_on(
              m_file_io.get_scheduler(),
              read_file(request, std::move(name), size, offset)) |
              stdexec::upon_error([](const std::exception_ptr&) noexcept {}),
          m_requests.get_token());
    } catch (...) {
      request.reset();  // The final request owner supplies a failure reply.
    }
  }
  void write(fuse_req_t req,
             fuse_ino_t,
             const char*,
             size_t size,
             off_t,
             fuse_file_info*) {
    if (!is_self_request(req)) {
      fuse_reply_err(req, EACCES);
    } else {
      fuse_reply_write(req, size);
    }
  }
  using Directory = FileHandle;
  void opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    const auto name = path(ino);
    auto entries = m_tree.ls(name);
    if (!entries) {
      fuse_reply_err(req, to_errno(entries.error()));
      return;
    }
    auto directory = std::make_unique<Directory>();
    directory->path = name;
    directory->names = {".", ".."};
    for (const auto& item : *entries) {
      directory->names.push_back(item.name.string());
      remember(name / item.name,
               std::holds_alternative<DirectoryInfo>(item.info));
    }
    fi->fh = retain(std::move(directory));
    if (fuse_reply_open(req, fi) != 0) {
      drop(fi->fh);
    }
  }
  // NOLINTBEGIN(bugprone-easily-swappable-parameters): libfuse callback
  // signature.
  void readdir(fuse_req_t req,
               fuse_ino_t,
               size_t size,
               off_t offset,
               fuse_file_info* fi) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const auto& names = file_handle(fi->fh).names;
    std::string buffer(size, '\0');
    size_t used = 0;
    for (auto i = static_cast<size_t>(offset); i < names.size(); ++i) {
      const struct stat info {};
      const size_t count =
          fuse_add_direntry(req, buffer.data() + used, size - used,
                            names[i].c_str(), &info, static_cast<off_t>(i + 1));
      if (count > size - used) {
        break;
      }
      used += count;
    }
    fuse_reply_buf(req, buffer.data(), used);
  }
  void releasedir(fuse_req_t req, fuse_ino_t, fuse_file_info* fi) {
    drop(fi->fh);
    fuse_reply_err(req, 0);
  }
  void create(fuse_req_t req,
              fuse_ino_t parent,
              const char* name,
              mode_t,
              fuse_file_info* fi) {
    const auto relative = path(parent) / name;
    if (!end_pretence(req, relative, Pretence::absent)) {
      fuse_reply_err(req, EACCES);
      return;
    }
    fi->direct_io = 1;
    entry(req, relative, fi);
  }
  void mknod(fuse_req_t req,
             fuse_ino_t parent,
             const char* name,
             mode_t,
             dev_t) {
    const auto relative = path(parent) / name;
    if (!end_pretence(req, relative, Pretence::absent)) {
      fuse_reply_err(req, EACCES);
      return;
    }
    entry(req, relative);
  }
  void mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t) {
    const auto relative = path(parent) / name;
    if (!end_pretence(req, relative, Pretence::absent)) {
      fuse_reply_err(req, EACCES);
      return;
    }
    entry(req, relative);
  }
  void unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
    fuse_reply_err(
        req, end_pretence(req, path(parent) / name, Pretence::present_file)
                 ? 0
                 : EACCES);
  }
  void rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
    fuse_reply_err(
        req, end_pretence(req, path(parent) / name, Pretence::present_directory)
                 ? 0
                 : EACCES);
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
  bool end_pretence(fuse_req_t req,
                    const std::filesystem::path& relative,
                    Pretence expected) {
    if (!is_self_request(req)) {
      return false;
    }
    const std::lock_guard<std::mutex> lock(m_pretence_mutex);
    if (m_pretence != expected || m_pretence_path != relative) {
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
