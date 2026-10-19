// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include "iocontext.hpp"
#include "processutilinternal.hpp"
#include "scopeguard.hpp"

#include <fuse_lowlevel.h>
#include <linux/openat2.h>
#include <exec/task.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/sched.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {errno, std::system_category()};
}

// RAII guard around a file descriptor.
class UniqueFd {
  int m_fd = -1;

 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : m_fd(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(UniqueFd&& other) noexcept : m_fd(other.release()) {}
  UniqueFd(const UniqueFd&) = delete;

  UniqueFd& operator=(UniqueFd other) noexcept {
    using std::swap;
    swap(*this, other);
    return *this;
  }

  [[nodiscard]] int get() const { return m_fd; }

  explicit operator bool() const { return m_fd >= 0; }

  // Gives up ownership without closing, returning the fd (or -1).
  int release() { return std::exchange(m_fd, -1); }

  // Closes the currently-owned fd, if any, and starts owning `fd` instead.
  void reset(int fd = -1) {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
    m_fd = fd;
  }

  friend void swap(UniqueFd& a, UniqueFd& b) noexcept {
    std::swap(a.m_fd, b.m_fd);
  }
};

// The /dev/fuse fd must be opened inside the child's new mount namespace - a
// pre-fork fd makes mount(2) fail with EINVAL - so the child hands it back over
// a socketpair via SCM_RIGHTS. The payload says whether setup succeeded, and if
// not, the errno of the step that failed: tracing is required, so that reason
// is what the run fails with.
struct Handoff {
  bool ok;
  int error;
};

// Sends @a fd, or - when @a fd is negative - that setup failed with @a error.
// Runs post-fork, so it allocates nothing.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void send_result(int sock, int fd, int error) {
  Handoff payload{.ok = fd >= 0, .error = error};
  iovec iov{.iov_base = &payload, .iov_len = sizeof(payload)};
  msghdr msg{.msg_iov = &iov, .msg_iovlen = 1};

  std::array<char, CMSG_SPACE(sizeof(int))> control{};
  if (fd >= 0) {
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
  }
  // Best-effort: the child is about to _exit() or execve() regardless.
  ::sendmsg(sock, &msg, 0);
}

// The fd the child handed over, or why it could not: the error it reported, or
// EPROTO if the handoff itself went wrong.
std::expected<UniqueFd, std::error_code> recv_result(int sock) {
  Handoff payload{.ok = false, .error = EPROTO};
  iovec iov{.iov_base = &payload, .iov_len = sizeof(payload)};
  std::array<char, CMSG_SPACE(sizeof(int))> control{};
  msghdr msg{.msg_iov = &iov,
             .msg_iovlen = 1,
             .msg_control = control.data(),
             .msg_controllen = control.size()};

  // Close-on-exec, like every descriptor the parent holds, so that no other
  // command launched meanwhile inherits it - which would keep this command's
  // FUSE connection alive for as long as that one lived.
  if (::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) !=
      static_cast<ssize_t>(sizeof(payload))) {
    return std::unexpected(std::make_error_code(std::errc::protocol_error));
  }
  if (!payload.ok) {
    return std::unexpected(
        std::error_code(payload.error, std::system_category()));
  }
  cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == nullptr || cmsg->cmsg_type != SCM_RIGHTS) {
    return std::unexpected(std::make_error_code(std::errc::protocol_error));
  }
  int fd = -1;
  std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
  return UniqueFd(fd);
}

// By view so nothing is allocated here: this runs post-fork, where allocating
// could deadlock on a malloc lock another thread held at fork time.
bool write_whole_file(const char* path, std::string_view data) {
  const int fd = ::open(path, O_WRONLY);
  if (fd < 0) {
    return false;
  }
  const bool ok = ::write(fd, data.data(), data.size()) ==
                  static_cast<ssize_t>(data.size());
  ::close(fd);
  return ok;
}

// Forks, as fork() does, but through clone3(2) with @a flags: returns the
// child's pid in the parent, 0 in the child, or -1 with errno set. With
// CLONE_PIDFD, @a pidfd receives a pidfd for the child. Unlike fork() it runs
// no atfork handlers - so a child of a multithreaded parent may call nothing
// that takes a lock, fork() included - and so it is safe to call again in such
// a child.
// NOLINTNEXTLINE(readability-non-const-parameter): clone3 writes to pidfd.
pid_t clone_process(std::uint64_t flags, int* pidfd = nullptr) {
  const clone_args args{.flags = flags,
                        .pidfd = std::bit_cast<std::uintptr_t>(pidfd),
                        .exit_signal = SIGCHLD};
  return static_cast<pid_t>(::syscall(SYS_clone3, &args, sizeof(args)));
}

// Opens @a relative under @a root_fd with @a flags, following no symlink and
// never leaving @a root_fd - not even through a directory swapped for a
// symlink since it was looked up.
UniqueFd open_beneath(int root_fd,
                      const std::filesystem::path& relative,
                      int flags) {
  const open_how how{
      .flags = static_cast<std::uint64_t>(flags | O_NOFOLLOW | O_CLOEXEC),
      .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS,
  };
  return UniqueFd(static_cast<int>(
      ::syscall(SYS_openat2, root_fd, relative.c_str(), &how, sizeof(how))));
}

// One inode in the tracer's passthrough view, keyed by a synthetic fuse_ino_t
// (root is always FUSE_ROOT_ID). Holds no fd: one per inode for the whole run
// would exhaust RLIMIT_NOFILE under a command that touches many files, so each
// request reopens it by its path instead, checking that it is still the same
// file.
struct Inode {
  std::filesystem::path relative;  // to the root; "." for the root itself
  std::filesystem::path real_path;
  dev_t dev;
  ino_t ino;
};

// Services one command's FUSE mount: a read-only passthrough view over the
// files rooted at the traced directory, recording the real path of every file
// the command opens for reading.
class Tracer {
  std::mutex m_mutex;
  std::vector<std::filesystem::path> m_read_files;

  // Set when a request could not be handled, which may have lost a read.
  std::atomic<bool> m_failed = false;
  UniqueFd m_root;
  std::unordered_map<fuse_ino_t, Inode> m_inodes;
  std::map<std::pair<dev_t, ino_t>, fuse_ino_t> m_by_dev_ino;
  fuse_ino_t m_next_ino = FUSE_ROOT_ID + 1;

  // The data of every read and listing, reused rather than allocated per
  // reply: a session serves one request at a time, so replies never overlap.
  std::vector<char> m_reply_buffer;

 public:
  // @pre @a root_fd is open on @a root.
  Tracer(int root_fd, const std::filesystem::path& root) : m_root(root_fd) {
    struct stat st {};
    ::fstat(root_fd, &st);
    m_inodes.try_emplace(FUSE_ROOT_ID, ".", root, st.st_dev, st.st_ino);
  }

  Tracer(const Tracer&) = delete;
  Tracer& operator=(const Tracer&) = delete;

  void lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    Inode* p = find(parent);
    if (p == nullptr) {
      fuse_reply_err(req, EIO);
      return;
    }
    const std::filesystem::path child = parent == FUSE_ROOT_ID
                                            ? std::filesystem::path(name)
                                            : p->relative / name;
    const UniqueFd fd = open_beneath(m_root.get(), child, O_PATH);
    if (!fd) {
      fuse_reply_err(req, errno);
      return;
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
      // Read errno before fd's destructor (whose close() may touch it) runs.
      fuse_reply_err(req, errno);
      return;
    }

    auto [it, inserted] =
        m_by_dev_ino.try_emplace(std::pair(st.st_dev, st.st_ino), m_next_ino);
    if (inserted) {
      ++m_next_ino;
      assert(!m_inodes.contains(it->second));
      m_inodes.try_emplace(it->second, child, p->real_path / name, st.st_dev,
                           st.st_ino);
    }

    const fuse_entry_param e = {
        .ino = it->second,
        .attr = st,
        .attr_timeout = 0,
        .entry_timeout = 0,
    };
    fuse_reply_entry(req, &e);
  }

  void getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* /*fi*/) {
    Inode* n = find(ino);
    if (n == nullptr) {
      fuse_reply_err(req, EIO);
      return;
    }
    struct stat st {};
    if (const UniqueFd fd = reopen(*n, O_PATH, st); !fd) {
      fuse_reply_err(req, errno);
      return;
    }
    fuse_reply_attr(req, &st, 0);
  }

  void open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    Inode* n = find(ino);
    if (n == nullptr) {
      fuse_reply_err(req, EIO);
      return;
    }
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
      fuse_reply_err(req, EROFS);
      return;
    }
    struct stat st {};
    UniqueFd fd = reopen(*n, O_RDONLY, st);
    if (!fd) {
      fuse_reply_err(req, errno);
      return;
    }

    {
      const std::scoped_lock lock(m_mutex);
      m_read_files.push_back(n->real_path);
    }

    // The fd now belongs to fi->fh; Tracer::release() closes it. Reads go
    // through the kernel's page cache - no direct_io, which would also refuse
    // the shared mmaps that compilers and linkers make of their inputs. That
    // loses no traced reads, since every open still comes here and is recorded
    // above. Nor can it show stale contents: each command's mount is new, so
    // its cache starts empty, and without keep_cache the kernel drops a file's
    // cached pages at every open, so a file rewritten while the command runs
    // reads as new at its next open.
    fi->fh = static_cast<std::uint64_t>(fd.release());
    fuse_reply_open(req, fi);
  }

  void read(fuse_req_t req,
            fuse_ino_t /*ino*/,
            size_t size,
            off_t off,
            fuse_file_info* fi) {
    if (m_reply_buffer.size() < size) {
      m_reply_buffer.resize(size);
    }
    const ssize_t n =
        ::pread(static_cast<int>(fi->fh), m_reply_buffer.data(), size, off);
    if (n < 0) {
      fuse_reply_err(req, errno);
      return;
    }
    fuse_reply_buf(req, m_reply_buffer.data(), static_cast<std::size_t>(n));
  }

  void release(fuse_req_t req, fuse_ino_t /*ino*/, fuse_file_info* fi) {
    ::close(static_cast<int>(fi->fh));
    fuse_reply_err(req, 0);
  }

  // Listing a directory - and reading a symlink, below - is not recorded as a
  // read, as on Windows, whose hook skips directories and records a symlink's
  // target rather than the link. Following a link inside the tree still opens
  // its target here, which records that.
  void opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    Inode* n = find(ino);
    if (n == nullptr) {
      fuse_reply_err(req, EIO);
      return;
    }
    struct stat st {};
    UniqueFd fd = reopen(*n, O_RDONLY | O_DIRECTORY, st);
    if (!fd) {
      fuse_reply_err(req, errno);
      return;
    }
    DIR* const dir = ::fdopendir(fd.get());
    if (dir == nullptr) {
      fuse_reply_err(req, errno);
      return;
    }
    fd.release();  // dir's now.

    // The stream now belongs to fi->fh; Tracer::releasedir() closes it.
    fi->fh = std::bit_cast<std::uintptr_t>(dir);
    fuse_reply_open(req, fi);
  }

  // libfuse's signature.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  void readdir(fuse_req_t req,
               fuse_ino_t /*ino*/,
               size_t size,
               off_t off,
               fuse_file_info* fi) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    DIR* const dir = directory(fi);
    // Offsets are those telldir() gave for an earlier batch, or 0.
    if (off == 0) {
      ::rewinddir(dir);
    } else {
      ::seekdir(dir, off);
    }
    if (m_reply_buffer.size() < size) {
      m_reply_buffer.resize(size);
    }
    char* const buf = m_reply_buffer.data();
    std::size_t used = 0;
    while (true) {
      errno = 0;
      const dirent* const entry = ::readdir(dir);
      if (entry == nullptr) {
        if (errno != 0 && used == 0) {
          fuse_reply_err(req, errno);
          return;
        }
        break;
      }
      // Only the type and inode number are used, for d_type and d_ino.
      const struct stat st {
        .st_ino = entry->d_ino,
        .st_mode = static_cast<mode_t>(entry->d_type) << 12,
      };
      const std::size_t needed = fuse_add_direntry(
          req, buf + used, size - used, entry->d_name, &st, ::telldir(dir));
      if (needed > size - used) {
        break;  // The next batch starts here, at the offset it asks for.
      }
      used += needed;
    }
    fuse_reply_buf(req, buf, used);
  }

  void releasedir(fuse_req_t req, fuse_ino_t /*ino*/, fuse_file_info* fi) {
    ::closedir(directory(fi));
    fuse_reply_err(req, 0);
  }

  void readlink(fuse_req_t req, fuse_ino_t ino) {
    Inode* n = find(ino);
    if (n == nullptr) {
      fuse_reply_err(req, EIO);
      return;
    }
    struct stat st {};
    const UniqueFd fd = reopen(*n, O_PATH, st);
    if (!fd) {
      fuse_reply_err(req, errno);
      return;
    }
    std::array<char, PATH_MAX + 1> target{};
    const ssize_t length =
        ::readlinkat(fd.get(), "", target.data(), target.size());
    if (length < 0) {
      fuse_reply_err(req, errno);
      return;
    }
    if (static_cast<std::size_t>(length) == target.size()) {
      fuse_reply_err(req, ENAMETOOLONG);
      return;
    }
    fuse_reply_readlink(req, target.data());
  }

  // Records that a request could not be handled.
  void fail() noexcept { m_failed = true; }

  // The files the command opened for reading, or EIO if a request could not
  // be handled - which may have lost one.
  std::expected<std::vector<std::filesystem::path>, std::error_code>
  take_read_files() && {
    if (m_failed) {
      return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    return std::move(m_read_files);
  }

 private:
  Inode* find(fuse_ino_t ino) {
    auto it = m_inodes.find(ino);
    return it == m_inodes.end() ? nullptr : &it->second;
  }

  // Opens @a n with @a flags, filling in @a st, or sets errno and returns no
  // fd - ESTALE if its path now names another file, as a rename or a delete
  // since its lookup can leave it.
  UniqueFd reopen(const Inode& n, int flags, struct stat& st) {
    UniqueFd fd = open_beneath(m_root.get(), n.relative, flags);
    if (!fd || ::fstat(fd.get(), &st) != 0) {
      return {};
    }
    if (st.st_dev != n.dev || st.st_ino != n.ino) {
      errno = ESTALE;
      return {};
    }
    return fd;
  }

  // The stream opendir() left in @a fi.
  static DIR* directory(const fuse_file_info* fi) {
    return std::bit_cast<DIR*>(static_cast<std::uintptr_t>(fi->fh));
  }
};

// Turns a Tracer member function into a plain C fuse_lowlevel_ops callback via
// fuse_req_userdata.
template <auto MemFn>
struct Trampoline;

template <typename... Args, void (Tracer::*MemFn)(fuse_req_t, Args...)>
struct Trampoline<MemFn> {
  static void call(fuse_req_t req, Args... args) {
    auto* tracer = static_cast<Tracer*>(fuse_req_userdata(req));
    try {
      (tracer->*MemFn)(req, args...);
    } catch (...) {
      tracer->fail();
      fuse_reply_err(req, EIO);
    }
  }
};

template <auto MemFn>
inline constexpr auto trampoline = Trampoline<MemFn>::call;

// Without auto_inval_data, which libfuse asks for by default: with attributes
// never cached, it would cost a getattr before every read the page cache could
// otherwise answer. Opens drop a file's cached pages anyway (see Tracer::open).
void init_tracer(void* /*userdata*/, fuse_conn_info* conn) {
  conn->want &= ~FUSE_CAP_AUTO_INVAL_DATA;
}

constexpr fuse_lowlevel_ops k_ops = {
    .init = init_tracer,
    .lookup = trampoline<&Tracer::lookup>,
    .getattr = trampoline<&Tracer::getattr>,
    .readlink = trampoline<&Tracer::readlink>,
    .open = trampoline<&Tracer::open>,
    .read = trampoline<&Tracer::read>,
    .release = trampoline<&Tracer::release>,
    .opendir = trampoline<&Tracer::opendir>,
    .readdir = trampoline<&Tracer::readdir>,
    .releasedir = trampoline<&Tracer::releasedir>,
};

ssize_t custom_io_read(int fd, void* buf, size_t buf_len, void* /*userdata*/) {
  return ::read(fd, buf, buf_len);
}

ssize_t custom_io_writev(int fd, iovec* iov, int count, void* /*userdata*/) {
  return ::writev(fd, iov, count);
}

constexpr fuse_custom_io k_io = {
    .writev = custom_io_writev,
    .read = custom_io_read,
    .splice_receive = nullptr,
    .splice_send = nullptr,
};

// Points stdout at the pipe and stdin at /dev/null, and puts the command in its
// own process group, out of reach of signals sent to ours - an interrupt at the
// terminal, say - which makebelieve handles by stopping the run instead. In a
// background group, a command reading our stdin - were it a terminal - would be
// stopped by SIGTTIN for good. Returns false on failure.
bool prepare_child_io(int stdout_fd) {
  if (::dup2(stdout_fd, STDOUT_FILENO) < 0) {
    return false;
  }
  ::close(stdout_fd);
  const int null_fd = ::open("/dev/null", O_RDONLY);
  if (null_fd < 0) {
    return false;
  }
  if (null_fd != STDIN_FILENO) {
    const bool redirected = ::dup2(null_fd, STDIN_FILENO) >= 0;
    ::close(null_fd);
    if (!redirected) {
      return false;
    }
  }
  ::setpgid(0, 0);
  return true;
}

// The status a shell would report for a process that ended with wait status
// @a status: its exit status, or 128 plus the signal that ended it.
int shell_status(int status) noexcept {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 0;
}

// Runs @a command in a child of this process, in @a cwd, and waits for it,
// reaping anything else that is reparented here meanwhile. Exits with the
// status the command ended with, as a shell reports it. Never returns.
//
// This process is the init of the command's pid namespace, so when it exits,
// the kernel kills everything left in the namespace - all that the command
// started, however it detached - and only lets this process be reaped once
// all of that has gone. The command runs as a child, rather than in this
// process, because the kernel shields an init from signals it has no handler
// for: as init, a shell could not even `kill $$`.
[[noreturn]] void run_as_init(const std::filesystem::path& cwd,
                              const std::string& command) {
  // This process never execs, so it would otherwise go on holding every
  // descriptor it was cloned with, close-on-exec or not - the parent's, and
  // so those of other commands running at the time - for as long as the
  // command ran. It keeps only the command's standard streams, for the shell
  // to inherit.
  ::close_range(STDERR_FILENO + 1, ~0U, 0);
  const pid_t shell = clone_process(0);
  if (shell < 0) {
    _exit(125);
  }
  if (shell == 0) {
    if (::chdir(cwd.c_str()) != 0) {
      _exit(127);
    }
    ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);  // Only reached if exec failed.
  }
  while (true) {
    int status = 0;
    const pid_t reaped = ::waitpid(-1, &status, 0);
    if (reaped == shell) {
      _exit(shell_status(status));
    }
    if (reaped < 0 && errno != EINTR) {
      _exit(125);
    }
  }
}

// The traced child, created by clone_process() in a fresh user, mount and pid
// namespace, of which it is the init: it sets up a private FUSE mount on @a cwd
// and then runs the command under it as run_as_init() does - or _exit()s. It
// never returns. Mounting directly on @a cwd keeps parent directories
// untracked, since ".." resolves out of the mount; only paths under @a cwd
// reach the tracer. Nothing in the child allocates or takes a lock, so uid_map
// and gid_map arrive pre-formatted.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[noreturn]] void run_traced_child(const std::filesystem::path& cwd,
                                   const std::string& command,
                                   int stdout_fd,
                                   std::string_view uid_map,
                                   std::string_view gid_map,
                                   int handoff_sock,
                                   int forced_error) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  // Reports why setup failed - read errno first, before anything can clobber
  // it - and exits.
  const auto fail = [handoff_sock](int error) {
    send_result(handoff_sock, -1, error);
    _exit(125);
  };

  if (forced_error != 0) {
    fail(forced_error);  // ProcessUtilTestUtil's tracing_setup.
  }
  if (!prepare_child_io(stdout_fd)) {
    fail(errno);
  }

  // The namespaces let the mounts below be made without real privilege, and
  // keep them invisible elsewhere.
  if (!write_whole_file("/proc/self/setgroups", "deny") ||
      !write_whole_file("/proc/self/uid_map", uid_map) ||
      !write_whole_file("/proc/self/gid_map", gid_map) ||
      // Detach mount propagation so nothing here leaks to or from the host.
      ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0 ||
      // A /proc of the command's own pid namespace, so that the ids its
      // processes see for themselves and each other are found there.
      ::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              nullptr) != 0) {
    fail(errno);
  }

  // Opened fresh, in the new namespaces - a pre-fork-opened /dev/fuse fd
  // inherited from the parent fails the mount(2) below with EINVAL.
  const int fuse_fd = ::open("/dev/fuse", O_RDWR);
  if (fuse_fd < 0) {
    fail(errno);
  }
  std::array<char, 256> mount_data{};
  std::snprintf(mount_data.data(), mount_data.size(),
                "fd=%d,rootmode=040000,user_id=0,group_id=0", fuse_fd);
  if (::mount("makebelieve-trace", cwd.c_str(), "fuse", MS_NOSUID | MS_NODEV,
              mount_data.data()) != 0) {
    fail(errno);
  }

  // Hand the fd off before resolving any path through the mount (chdir() does):
  // until the parent services it, such a lookup would hang forever.
  send_result(handoff_sock, fuse_fd, 0);
  ::close(fuse_fd);

  // Wait until the parent is servicing the mount before resolving a path
  // through it.
  char ack = 0;
  if (::read(handoff_sock, &ack, 1) != 1) {
    _exit(125);
  }
  ::close(handoff_sock);

  run_as_init(cwd, command);
}

// A running command: the init of its own pid namespace, which runs the
// command's shell and exits with its status (see run_as_init()). It exits -
// and its pidfd becomes readable - only once everything else in the namespace
// has gone, and killing it kills all of that. So terminate(), which kills it
// and waits for it to exit, through the IoContext, before reaping it, ends
// everything the command started and waits for all of it. Destroying one that
// has not been reaped does the same, blocking, as a last resort.
class ChildProcess {
  pid_t m_pid;

  // Readable once the command, and everything it started, has exited.
  UniqueFd m_exited;

  bool m_reaped = false;
  int m_status = 0;

 public:
  ChildProcess(pid_t pid, UniqueFd pidfd)
      : m_pid(pid), m_exited(std::move(pidfd)) {}

  ~ChildProcess() {
    if (!m_reaped) {
      signal();
      reap();
    }
  }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&&) = delete;
  ChildProcess& operator=(ChildProcess&&) = delete;

  // Readable once the command, and everything it started, has exited.
  [[nodiscard]] int exited() const noexcept { return m_exited.get(); }

  // Kills the command and everything it started. Touches nothing but the
  // numbers it was started with, so it is safe from any thread - until the
  // command is reaped, after which its id may be reused.
  // @pre The command has not been reaped.
  void signal() const noexcept { ::kill(m_pid, SIGKILL); }

  // Waits for the command, which has exited or been killed, recording how it
  // ended. Blocks until everything it started has gone, too.
  void reap() noexcept {
    while (::waitpid(m_pid, &m_status, 0) < 0 && errno == EINTR) {
    }
    m_reaped = true;
  }

  // How the command ended, as a shell reports it: its exit status, or 128
  // plus the signal that ended it. @pre It has been reaped.
  [[nodiscard]] int exit_status() const noexcept {
    return shell_status(m_status);
  }

  // Kills the command and everything it started, then waits for all of it to
  // exit through @a io and reaps the command - which then no longer blocks.
  exec::task<void> terminate(IoContext& io) {
    if (m_reaped) {
      co_return;
    }
    signal();
    co_await (stdexec::write_env(io.async_wait(m_exited.get()),
                                 stdexec::prop{stdexec::get_stop_token,
                                               stdexec::never_stop_token{}}) |
              stdexec::upon_error([](std::error_code) noexcept {}));
    reap();
  }
};

// Kills @a child when a stop is requested. Only ever registered while the
// child has yet to be reaped.
struct Kill {
  const ChildProcess* child;

  void operator()() const noexcept { child->signal(); }
};

// What supervising a running command produced: its stdout, plus how it ended.
struct Supervision {
  std::string output;
  std::error_code error;  // set if waiting on the child failed
  bool canceled =
      false;            // true if it was terminated because stop was requested
  int exit_status = 0;  // as ChildProcess::exit_status() reports it
};

// What ends a command early.
struct KillOn {
  stdexec::inplace_stop_token stop;   // the run's: it was cancelled
  stdexec::inplace_stop_token abort;  // something found it cannot succeed
};

// Collects @a child's stdout from @a read_fd until it exits, then reaps it,
// killing it if either of @a kill_on is requested. Waits on both through
// @a io. This is the child's one owner while it runs: nothing else signals or
// reaps it. Knows nothing of tracing.
exec::task<Supervision> supervise(IoContext& io,
                                  ChildProcess& child,
                                  UniqueFd read_fd,
                                  KillOn kill_on) {
  Supervision result;

  // Non-blocking, so draining stops at what is there now.
  ::fcntl(read_fd.get(), F_SETFL, ::fcntl(read_fd.get(), F_GETFL) | O_NONBLOCK);
  std::array<char, 4096> buffer{};
  // Reads what the pipe holds now; false once it has reached end of file.
  const auto drain = [&]() {
    while (true) {
      const ssize_t count = ::read(read_fd.get(), buffer.data(), buffer.size());
      if (count > 0) {
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
      } else if (count == 0) {
        return false;
      } else if (errno != EINTR) {
        return errno == EAGAIN;
      }
    }
  };

  // Collect output until the direct child exits. Waiting on the child rather
  // than on the pipe closing is what keeps a grandchild that inherited the
  // pipe from wedging us - the child exits once its shell has, and all else
  // is then killed - and reading as it arrives keeps a full pipe from
  // blocking the command.
  std::error_code wait_error;
  bool open = true;
  {
    // Gone before the child is reaped - their destruction waiting out one
    // running on another thread - so neither can signal a process that has
    // reused its id.
    const stdexec::inplace_stop_callback<Kill> on_stop(kill_on.stop,
                                                       Kill{&child});
    const stdexec::inplace_stop_callback<Kill> on_abort(kill_on.abort,
                                                        Kill{&child});
    const auto failed = [&wait_error](std::error_code error) noexcept {
      wait_error = error;
      return -1;
    };
    constexpr int k_output = 0;
    constexpr int k_exited = 1;
    while (true) {
      auto child_exited = io.async_wait(child.exited()) |
                          stdexec::then([]() noexcept { return k_exited; }) |
                          stdexec::upon_error(failed);
      const int which =
          open ? co_await exec::when_any(
                     io.async_wait(read_fd.get()) |
                         stdexec::then([]() noexcept { return k_output; }) |
                         stdexec::upon_error(failed),
                     child_exited)
               : co_await child_exited;
      if (which != k_output) {
        break;
      }
      open = drain();
    }
    if (wait_error) {
      child.signal();
    }
  }

  child.reap();
  result.exit_status = child.exit_status();
  if (open) {
    drain();  // Whatever the child wrote just before it exited.
  }

  result.error = wait_error;
  result.canceled = !wait_error && kill_on.stop.stop_requested();
  co_return result;
}

// Turns a Supervision into the reported Result. @a inputs is empty on a
// canceled or failed run.
ProcessUtil::Result to_result(Supervision supervision,
                              std::vector<std::filesystem::path> inputs) {
  if (supervision.error) {
    return std::unexpected(supervision.error);
  }
  if (supervision.canceled) {
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));
  }
  return ProcessUtil::Output{.standard_output = std::move(supervision.output),
                             .inputs = std::move(inputs),
                             .exit_status = supervision.exit_status};
}

// Traces the files one command reads under a root: the FUSE session behind the
// mount the command's own namespace sees there, answering its requests as a
// read-only passthrough and recording what it opens.
class TracingSession {
  Tracer m_tracer;
  std::string m_program = "makebelieve-trace";
  std::array<char*, 1> m_argv{m_program.data()};
  fuse_args m_args = FUSE_ARGS_INIT(1, m_argv.data());
  fuse_session* m_session = nullptr;
  int m_descriptor;

  // Allocated by libfuse on the first receive.
  fuse_buf m_buffer{};

  // ProcessUtilTestUtil's tracing_service, due at the first request.
  std::optional<std::error_code> m_forced_error;

  TracingSession(int root_fd, const std::filesystem::path& root, int descriptor)
      : m_tracer(root_fd, root), m_descriptor(descriptor) {}

  // Whether a session has more requests to come.
  enum class Serving : bool { more, ended };

  // Handles every request waiting, reporting whether more may come - or why
  // serving failed, in which case requests may have gone unanswered and reads
  // unrecorded. (The mount going away with the command reads as the session
  // ending, not as an error.)
  std::expected<Serving, std::error_code> process() noexcept {
    if (m_forced_error.has_value()) {
      const std::error_code forced = *m_forced_error;
      m_forced_error.reset();
      return std::unexpected(forced);
    }
    while (!fuse_session_exited(m_session)) {
      const int received = fuse_session_receive_buf(m_session, &m_buffer);
      if (received == -EAGAIN) {
        return Serving::more;
      }
      if (received == -EINTR) {
        continue;
      }
      if (received < 0) {
        return std::unexpected(
            std::error_code(-received, std::system_category()));
      }
      if (received == 0) {
        return Serving::ended;
      }
      fuse_session_process_buf(m_session, &m_buffer);
    }
    return Serving::ended;
  }

 public:
  // Serves the mount behind @a fuse_fd as a passthrough onto @a root, or says
  // why it cannot.
  static std::expected<std::unique_ptr<TracingSession>, std::error_code> open(
      const std::filesystem::path& root,
      UniqueFd fuse_fd) {
    // The parent's own passthrough root, resolved in the parent's namespace
    // and so unaffected by the child's private mount.
    const int root_fd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
      return std::unexpected(last_error_code());
    }
    std::unique_ptr<TracingSession> session(
        new TracingSession(root_fd, root, fuse_fd.get()));

    session->m_session =
        fuse_session_new(&session->m_args, &k_ops, sizeof(fuse_lowlevel_ops),
                         &session->m_tracer);
    if (session->m_session == nullptr) {
      return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    // Non-blocking, so serving stops at the requests there are now.
    ::fcntl(fuse_fd.get(), F_SETFL,
            ::fcntl(fuse_fd.get(), F_GETFL) | O_NONBLOCK);
    if (const int failed =
            fuse_session_custom_io(session->m_session, &k_io, fuse_fd.get());
        failed != 0) {
      return std::unexpected(
          std::error_code(failed < 0 ? -failed : EIO, std::system_category()));
    }
    fuse_fd.release();  // The session's now.
    return session;
  }

  ~TracingSession() {
    std::free(m_buffer.mem);
    if (m_session != nullptr) {
      fuse_session_destroy(m_session);
    }
    fuse_opt_free_args(&m_args);
  }

  TracingSession(const TracingSession&) = delete;
  TracingSession& operator=(const TracingSession&) = delete;
  TracingSession(TracingSession&&) = delete;
  TracingSession& operator=(TracingSession&&) = delete;

  // Serves the command's requests as they arrive, until @a stop is requested
  // or the session ends - completing with nothing - or serving fails,
  // completing with why: waiting on the device failed, or reading from it did.
  // Answering a request takes real filesystem calls, which slow storage can
  // stretch out, so they run where this task resumes - a worker - rather than
  // on the IoContext's thread, where they would hold up every other wait. One
  // loop per session, so its requests are served one batch at a time.
  exec::task<std::error_code> serve(IoContext& io,
                                    stdexec::inplace_stop_token stop) {
    while (true) {
      std::error_code wait_error;
      const bool stopped = co_await (
          stdexec::write_env(io.async_wait(m_descriptor),
                             stdexec::prop{stdexec::get_stop_token, stop}) |
          stdexec::then([]() noexcept { return false; }) |
          stdexec::upon_error([&wait_error](std::error_code error) noexcept {
            wait_error = error;
            return false;
          }) |
          stdexec::upon_stopped([]() noexcept { return true; }));
      if (stopped) {
        co_return std::error_code{};
      }
      if (wait_error) {
        co_return wait_error;
      }
      const std::expected<Serving, std::error_code> served = process();
      if (!served.has_value()) {
        co_return served.error();
      }
      if (*served == Serving::ended) {
        co_return std::error_code{};
      }
    }
  }

  // Ends the session, so that serving it ends too.
  void end() noexcept { fuse_session_exit(m_session); }

  // Makes serving fail with @a error at the first request. For
  // ProcessUtilTestUtil.
  void fail_at_first_request(std::error_code error) noexcept {
    m_forced_error = error;
  }

  // The files the command opened for reading, sorted and without repeats -
  // or why a request could not be handled, in which case what was recorded
  // may be incomplete and is not reported at all.
  [[nodiscard]] std::expected<std::vector<std::filesystem::path>,
                              std::error_code>
  take_inputs() && {
    std::expected<std::vector<std::filesystem::path>, std::error_code> inputs =
        std::move(m_tracer).take_read_files();
    if (inputs.has_value()) {
      std::ranges::sort(*inputs);
      inputs->erase(std::ranges::unique(*inputs).begin(), inputs->end());
    }
    return inputs;
  }
};

// Supervises @a child - its one owner - then ends its tracing @a session and
// stops @a stop_serving, however supervising went, so that serving always
// ends too.
exec::task<Supervision> supervise_then_end(
    IoContext& io,
    ChildProcess& child,
    UniqueFd read_fd,
    KillOn kill_on,
    TracingSession& session,
    stdexec::inplace_stop_source& stop_serving) {
  Supervision supervision;
  try {
    supervision = co_await supervise(io, child, std::move(read_fd), kill_on);
  } catch (...) {
    supervision.error = std::make_error_code(std::errc::not_enough_memory);
  }
  // Reaped by now, and with it everything it started, so nothing is left to
  // make a request.
  session.end();
  stop_serving.request_stop();
  co_return supervision;
}

// Serves @a session until @a stop is requested or it ends. If serving fails,
// records why in @a failure and requests @a abort, which has supervision end
// the command: a command whose mount goes unserved would block on it for
// ever, and its build has failed anyway. Never touches the child itself.
exec::task<void> serve_or_abort(IoContext& io,
                                TracingSession& session,
                                stdexec::inplace_stop_token stop,
                                stdexec::inplace_stop_source& abort,
                                std::error_code& failure) {
  std::error_code error;
  try {
    error = co_await session.serve(io, stop);
  } catch (...) {
    error = std::make_error_code(std::errc::not_enough_memory);
  }
  if (error) {
    failure = error;
    abort.request_stop();
  }
}

// Setting up tracing, which spans the command's launch, since the FUSE device
// has to be opened inside the command's own namespace:
//
// 1. prepare(), before the launch: a socket pair for the child to hand the
//    device back over, and its uid and gid maps, formatted now because the
//    child may not allocate.
// 2. run_child(), in the child, which starts in fresh namespaces: mounts the
//    device over the root, hands it back and waits.
// 3. attach(), in the parent: serves the device it was handed, or reports why
//    the child could not set up; the command is then added to the build
//    process tracker, and only once that succeeds does release() tell the
//    child the session is ready, so only then does the command run.
class TracingSetup {
  UniqueFd m_parent;
  UniqueFd m_child;
  std::string m_uid_map;
  std::string m_gid_map;

  TracingSetup(UniqueFd parent, UniqueFd child)
      : m_parent(std::move(parent)),
        m_child(std::move(child)),
        // Map the invoking user to root in the new user namespace.
        m_uid_map("0 " + std::to_string(::getuid()) + " 1"),
        m_gid_map("0 " + std::to_string(::getgid()) + " 1") {}

 public:
  static std::expected<TracingSetup, std::error_code> prepare() {
    std::array<int, 2> sv{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()) != 0) {
      return std::unexpected(last_error_code());
    }
    return TracingSetup(UniqueFd(sv[0]), UniqueFd(sv[1]));
  }

  // In the child: sets up tracing under @a root and, once released, runs
  // @a command with its standard output on @a stdout_fd - or, if
  // @a forced_error is set, fails setup with it first. Never returns.
  [[noreturn]] void run_child(const std::filesystem::path& root,
                              const std::string& command,
                              int stdout_fd,
                              int forced_error) {
    m_parent.reset();
    run_traced_child(root, command, stdout_fd, m_uid_map, m_gid_map,
                     m_child.release(), forced_error);
  }

  // In the parent, once the child is launched: lets go of its end.
  void launched() noexcept { m_child.reset(); }

  // Serves the device the child handed back as a passthrough onto @a root, or
  // reports why the child could not set up.
  std::expected<std::unique_ptr<TracingSession>, std::error_code> attach(
      const std::filesystem::path& root) {
    std::expected<UniqueFd, std::error_code> fuse_fd =
        recv_result(m_parent.get());
    if (!fuse_fd.has_value()) {
      return std::unexpected(fuse_fd.error());
    }
    return TracingSession::open(root, std::move(*fuse_fd));
  }

  // Tells the child the session is ready, so its command may run.
  void release() noexcept {
    const char ack = 1;
    const ssize_t acked = ::write(m_parent.get(), &ack, 1);
    static_cast<void>(acked);
    m_parent.reset();
  }
};

// Runs @a command under a FUSE passthrough on @a root, reporting the absolute
// paths it read under @a root. Tracing is required: if it cannot be set up -
// no unprivileged user namespaces, say - or fails while the command runs, the
// run fails with the reason rather than reporting no inputs.
exec::task<ProcessUtil::Result> run_traced(IoContext& io,
                                           std::filesystem::path root,
                                           std::string command,
                                           BuildProcessTracker& tracker,
                                           std::filesystem::path output,
                                           stdexec::inplace_stop_token stop) {
  // Close-on-exec, so that only this command gets the write end - as its
  // stdout, which dup2() leaves inheritable - and no other launched meanwhile
  // gets either end.
  std::array<int, 2> out_pipe{-1, -1};
  if (::pipe2(out_pipe.data(), O_CLOEXEC) != 0) {
    co_return std::unexpected(last_error_code());
  }
  UniqueFd out_read(out_pipe[0]);
  UniqueFd out_write(out_pipe[1]);

  std::expected<TracingSetup, std::error_code> setup = TracingSetup::prepare();
  if (!setup.has_value()) {
    co_return std::unexpected(setup.error());
  }

  // Taken before the fork, so the parent consumes it and the child sees it.
  const int forced_setup_error =
      detail::take_forced_failure(ProcessUtilTestUtil::Failure::tracing_setup)
          .value_or(std::error_code{})
          .value();

  // The command's record in the tracker, once added. Removed only once the
  // command, and everything it started, has been reaped - declared before the
  // child so it is removed only after that.
  std::optional<BuildProcessTracker::Token> added_as;
  const ScopeGuard remove_from_tracker([&tracker, &added_as] {
    if (added_as.has_value()) {
      tracker.remove(*added_as);
    }
  });

  int pidfd = -1;
  const pid_t pid = clone_process(
      CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_PIDFD, &pidfd);
  if (pid < 0) {
    co_return std::unexpected(last_error_code());
  }
  if (pid == 0) {
    out_read.reset();
    setup->run_child(root, command, out_write.get(), forced_setup_error);
  }
  ChildProcess child(pid, UniqueFd(pidfd));
  setup->launched();
  out_write.reset();  // Only the child writes stdout.

  std::expected<std::unique_ptr<TracingSession>, std::error_code> session =
      setup->attach(root);
  if (!session.has_value()) {
    co_await child.terminate(io);
    co_return std::unexpected(session.error());
  }
  if (const std::optional<std::error_code> forced = detail::take_forced_failure(
          ProcessUtilTestUtil::Failure::tracing_service)) {
    (*session)->fail_at_first_request(*forced);
  }
  // The child is parked at the barrier, before exec, so the command has done
  // nothing yet; and it is our unreaped child, so its pid is still its own.
  const std::expected<BuildProcessTracker::Token, std::error_code> added =
      tracker.add(output, ProcessHandle{.pid = static_cast<std::uint32_t>(pid),
                                        .pidfd = child.exited()});
  if (!added.has_value()) {
    co_await child.terminate(io);
    co_return std::unexpected(added.error());
  }
  added_as = *added;
  setup->release();

  // Supervise the command and serve its requests side by side. Supervision
  // alone owns the child; serving that fails tells it to end the command
  // through `abort`. Serving has ended, and so is done with the session, by
  // the time both have completed.
  stdexec::inplace_stop_source stop_serving;
  stdexec::inplace_stop_source abort;
  std::error_code tracing_failure;
  Supervision supervision = co_await stdexec::when_all(
      supervise_then_end(io, child, std::move(out_read),
                         {.stop = stop, .abort = abort.get_token()}, **session,
                         stop_serving),
      serve_or_abort(io, **session, stop_serving.get_token(), abort,
                     tracing_failure));
  if (tracing_failure) {
    co_return std::unexpected(tracing_failure);
  }
  if (supervision.error || supervision.canceled) {
    co_return to_result(std::move(supervision), {});
  }

  std::expected<std::vector<std::filesystem::path>, std::error_code> inputs =
      std::move(**session).take_inputs();
  if (!inputs.has_value()) {
    co_return std::unexpected(inputs.error());
  }
  co_return to_result(std::move(supervision), std::move(*inputs));
}

}  // namespace

exec::task<ProcessUtil::Result> ProcessUtil::run_task(
    IoContext& io,
    std::filesystem::path working_directory,
    std::string command,
    BuildProcessTracker& tracker,
    std::filesystem::path output,
    stdexec::inplace_stop_token stop) {
  if (stop.stop_requested()) {
    co_return std::unexpected(
        std::make_error_code(std::errc::operation_canceled));
  }

  // Canonicalize the root so the reported paths line up when the working
  // directory is reached through a symlink (a symlinked temp dir being common)
  // and survive the caller's own relativization.
  std::error_code ec;
  std::filesystem::path root =
      std::filesystem::canonical(working_directory, ec);
  if (ec) {
    root = working_directory;
  }
  co_return co_await run_traced(io, std::move(root), std::move(command),
                                tracker, std::move(output), stop);
}

}  // namespace makebelieve
