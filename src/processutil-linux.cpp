// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fuse_lowlevel.h>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
// a socketpair via SCM_RIGHTS. The one-byte payload also signals setup success:
// kHandoffOk carries the fd; kHandoffFailed means none follows.
constexpr char k_handoff_ok = 1;
constexpr char k_handoff_failed = 0;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void send_result(int sock, int fd) {
  char payload = fd >= 0 ? k_handoff_ok : k_handoff_failed;
  iovec iov{.iov_base = &payload, .iov_len = 1};
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

// Returns the received fd, or -1 if the child reported failure or the handoff
// protocol was otherwise violated.
int recv_result(int sock) {
  char payload = 0;
  iovec iov{.iov_base = &payload, .iov_len = 1};
  std::array<char, CMSG_SPACE(sizeof(int))> control{};
  msghdr msg{.msg_iov = &iov,
             .msg_iovlen = 1,
             .msg_control = control.data(),
             .msg_controllen = control.size()};

  if (::recvmsg(sock, &msg, 0) <= 0 || payload != k_handoff_ok) {
    return -1;
  }
  cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == nullptr || cmsg->cmsg_type != SCM_RIGHTS) {
    return -1;
  }
  int fd = -1;
  std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
  return fd;
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

// One O_PATH-fd-backed inode in the tracer's passthrough view, keyed by a
// synthetic fuse_ino_t (root is always FUSE_ROOT_ID). Owns its fd via UniqueFd,
// so destroying m_inodes closes every fd for free.
struct Inode {
  UniqueFd fd;
  std::filesystem::path real_path;
};

// Services one command's FUSE mount: a read-only passthrough view over the
// files rooted at the traced directory, recording the real path of every file
// the command opens for reading.
class Tracer {
  std::mutex m_mutex;
  std::vector<std::filesystem::path> m_read_files;
  std::unordered_map<fuse_ino_t, Inode> m_inodes;
  std::map<std::pair<dev_t, ino_t>, fuse_ino_t> m_by_dev_ino;
  fuse_ino_t m_next_ino = FUSE_ROOT_ID + 1;

 public:
  Tracer(int root_fd, std::filesystem::path root) {
    Inode& r = m_inodes[FUSE_ROOT_ID];
    r.fd = UniqueFd(root_fd);
    r.real_path = std::move(root);
  }

  Tracer(const Tracer&) = delete;
  Tracer& operator=(const Tracer&) = delete;

  void lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    Inode* p = find(parent);
    if (p == nullptr) {
      fuse_reply_err(req, EIO);
      return;
    }
    const std::filesystem::path child_path = p->real_path / name;
    UniqueFd fd(::openat(p->fd.get(), name, O_PATH | O_NOFOLLOW));
    if (!fd) {
      fuse_reply_err(req, errno);
      return;
    }
    struct stat st {};
    if (::fstatat(fd.get(), "", &st, AT_EMPTY_PATH) != 0) {
      // Read errno before fd's destructor (whose close() may touch it) runs.
      fuse_reply_err(req, errno);
      return;
    }

    auto [it, inserted] =
        m_by_dev_ino.try_emplace(std::pair(st.st_dev, st.st_ino), m_next_ino);
    if (inserted) {
      ++m_next_ino;
      assert(!m_inodes.contains(it->second));
      m_inodes.try_emplace(it->second, std::move(fd), child_path);
    } else {
      fd.reset();
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
    if (::fstatat(n->fd.get(), "", &st, AT_EMPTY_PATH) != 0) {
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
    // An O_PATH fd reopens via "." only for directories; /proc/self/fd/N
    // reopens a regular file too.
    std::array<char, 32> proc_fd_path{};
    std::snprintf(proc_fd_path.data(), proc_fd_path.size(), "/proc/self/fd/%d",
                  n->fd.get());
    UniqueFd fd(::open(proc_fd_path.data(), O_RDONLY));
    if (!fd) {
      fuse_reply_err(req, errno);
      return;
    }

    {
      const std::scoped_lock lock(m_mutex);
      m_read_files.push_back(n->real_path);
    }

    // The fd now belongs to fi->fh; Tracer::release() closes it.
    fi->fh = static_cast<std::uint64_t>(fd.release());
    fi->direct_io = 1;
    fuse_reply_open(req, fi);
  }

  void read(fuse_req_t req,
            fuse_ino_t /*ino*/,
            size_t size,
            off_t off,
            fuse_file_info* fi) {
    std::vector<char> buf(size);
    const ssize_t n = ::pread(static_cast<int>(fi->fh), buf.data(), size, off);
    if (n < 0) {
      fuse_reply_err(req, errno);
      return;
    }
    fuse_reply_buf(req, buf.data(), static_cast<std::size_t>(n));
  }

  void release(fuse_req_t req, fuse_ino_t /*ino*/, fuse_file_info* fi) {
    ::close(static_cast<int>(fi->fh));
    fuse_reply_err(req, 0);
  }

  std::vector<std::filesystem::path> take_read_files() && {
    return std::move(m_read_files);
  }

 private:
  Inode* find(fuse_ino_t ino) {
    auto it = m_inodes.find(ino);
    return it == m_inodes.end() ? nullptr : &it->second;
  }
};

// Turns a Tracer member function into a plain C fuse_lowlevel_ops callback via
// fuse_req_userdata.
template <auto MemFn>
struct Trampoline;

template <typename... Args, void (Tracer::*MemFn)(fuse_req_t, Args...)>
struct Trampoline<MemFn> {
  static void call(fuse_req_t req, Args... args) {
    try {
      auto* tracer = static_cast<Tracer*>(fuse_req_userdata(req));
      (tracer->*MemFn)(req, args...);
    } catch (...) {
      fuse_reply_err(req, EIO);
    }
  }
};

template <auto MemFn>
inline constexpr auto trampoline = Trampoline<MemFn>::call;

constexpr fuse_lowlevel_ops k_ops = {
    .lookup = trampoline<&Tracer::lookup>,
    .getattr = trampoline<&Tracer::getattr>,
    .open = trampoline<&Tracer::open>,
    .read = trampoline<&Tracer::read>,
    .release = trampoline<&Tracer::release>,
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

template <typename T, typename D>
std::unique_ptr<T, D> make_scope_ptr(T* p, D deleter) {
  return std::unique_ptr<T, D>(p, deleter);
}

// Points stdout at the pipe and puts the command in its own process group, so a
// stop can signal the whole tree it spawns. Returns false on failure.
bool prepare_child_io(int stdout_fd) {
  if (::dup2(stdout_fd, STDOUT_FILENO) < 0) {
    return false;
  }
  ::close(stdout_fd);
  ::setpgid(0, 0);
  return true;
}

// The traced child: either execve()s the command inside its private FUSE mount
// or _exit()s - it never returns. Mounting directly on @a cwd keeps parent
// directories untracked, since ".." resolves out of the mount; only paths under
// @a cwd reach the tracer. Nothing between fork and exec allocates, so uid_map
// and gid_map arrive pre-formatted.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[noreturn]] void run_traced_child(const std::filesystem::path& cwd,
                                   const std::string& command,
                                   int stdout_fd,
                                   std::string_view uid_map,
                                   std::string_view gid_map,
                                   int handoff_sock) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (!prepare_child_io(stdout_fd)) {
    send_result(handoff_sock, -1);
    _exit(125);
  }

  // A fresh user + mount namespace, so the mount below is invisible elsewhere
  // and can be made without real privilege.
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0 ||
      !write_whole_file("/proc/self/setgroups", "deny") ||
      !write_whole_file("/proc/self/uid_map", uid_map) ||
      !write_whole_file("/proc/self/gid_map", gid_map) ||
      // Detach mount propagation so nothing here leaks to or from the host.
      ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    send_result(handoff_sock, -1);
    _exit(125);
  }

  // Opened fresh, post-unshare - a pre-fork-opened /dev/fuse fd inherited from
  // the parent fails the mount(2) below with EINVAL.
  const int fuse_fd = ::open("/dev/fuse", O_RDWR);
  std::array<char, 256> mount_data{};
  std::snprintf(mount_data.data(), mount_data.size(),
                "fd=%d,rootmode=040000,user_id=0,group_id=0", fuse_fd);
  if (fuse_fd < 0 || ::mount("makebelieve-trace", cwd.c_str(), "fuse",
                             MS_NOSUID | MS_NODEV, mount_data.data()) != 0) {
    send_result(handoff_sock, -1);
    _exit(125);
  }

  // Hand the fd off before resolving any path through the mount (chdir() does):
  // until the parent services it, such a lookup would hang forever.
  send_result(handoff_sock, fuse_fd);
  ::close(fuse_fd);

  // Wait until the parent is servicing the mount before resolving a path
  // through it.
  char ack = 0;
  if (::read(handoff_sock, &ack, 1) != 1) {
    _exit(125);
  }
  ::close(handoff_sock);

  if (::chdir(cwd.c_str()) != 0) {
    _exit(127);
  }
  ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
  _exit(127);  // Only reached if exec failed.
}

// What supervising a running command produced: its stdout, plus how it ended.
struct Supervision {
  std::string output;
  std::error_code error;  // set if waiting on the child failed
  bool canceled =
      false;  // true if it was terminated because stop was requested
};

// Drains @a child's stdout from @a read_fd until it exits, terminating its
// process group if @a stop is requested. Takes ownership of @a read_fd and
// closes it. @a child must lead its own process group.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Supervision supervise(pid_t child, int read_fd, const std::stop_token& stop) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  // Read without blocking so a grandchild that inherited the pipe cannot keep
  // us waiting once our direct child has exited.
  ::fcntl(read_fd, F_SETFL, ::fcntl(read_fd, F_GETFL) | O_NONBLOCK);

  // Terminate the whole command group on stop. The callback runs synchronously
  // from the constructor if @a stop is already requested.
  const std::stop_callback on_stop(stop, [child] {
    ::kill(-child, SIGKILL);
    ::kill(child, SIGKILL);
  });

  std::string output;
  std::array<char, 4096> buffer{};
  const auto drain = [&] {
    while (true) {
      const ssize_t count = ::read(read_fd, buffer.data(), buffer.size());
      if (count <= 0) {
        return;  // No data available right now, or end of file.
      }
      output.append(buffer.data(), static_cast<std::size_t>(count));
    }
  };

  // Drain the pipe until the direct child exits. Waiting on the child rather
  // than on pipe end-of-file is what keeps an inherited-pipe grandchild from
  // wedging us, and draining as we go keeps a full pipe from blocking writes.
  std::error_code wait_error;
  while (true) {
    pollfd descriptor{.fd = read_fd, .events = POLLIN, .revents = 0};
    ::poll(&descriptor, 1,
           100);  // Wake on data or hangup, or re-check at 100ms.
    drain();

    int status = 0;
    const pid_t reaped = ::waitpid(child, &status, WNOHANG);
    if (reaped == child) {
      break;
    }
    if (reaped < 0 && errno != EINTR) {
      wait_error = last_error_code();
      break;
    }
  }
  drain();  // Whatever the child buffered just before it exited.
  ::close(read_fd);

  Supervision result{.output = std::move(output)};
  if (wait_error) {
    result.error = wait_error;
  } else if (stop.stop_requested()) {
    result.canceled = true;
  }
  return result;
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
                             .inputs = std::move(inputs)};
}

// Runs @a command under a FUSE passthrough on @a root, returning the absolute
// paths it read under @a root. std::nullopt means the tracer could not be set
// up (e.g. no user namespaces), so the caller falls back to an untraced run.
std::optional<ProcessUtil::Result> run_with_tracing(
    const std::filesystem::path& root,
    const std::string& command,
    const std::stop_token& stop) {
  std::array<int, 2> out_pipe{-1, -1};
  if (::pipe(out_pipe.data()) != 0) {
    return std::nullopt;
  }
  UniqueFd out_read(out_pipe[0]);
  UniqueFd out_write(out_pipe[1]);

  std::array<int, 2> sv{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) != 0) {
    return std::nullopt;
  }
  UniqueFd parent_socket(sv[0]);
  UniqueFd child_socket(sv[1]);

  // Map the invoking user to root in the new user namespace, formatted before
  // the fork so the child need not allocate.
  const std::string uid_map = "0 " + std::to_string(::getuid()) + " 1";
  const std::string gid_map = "0 " + std::to_string(::getgid()) + " 1";

  const pid_t pid = ::fork();
  if (pid < 0) {
    return std::nullopt;
  }
  if (pid == 0) {
    parent_socket.reset();
    out_read.reset();
    run_traced_child(root, command, out_write.get(), uid_map, gid_map,
                     child_socket.release());
  }
  child_socket.reset();
  out_write.reset();  // Only the child writes stdout.

  // Reaps the child after a setup failure: it is exiting, or blocked on the ack
  // that will now never come, so a kill unblocks it either way.
  const auto reap = [pid] {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
  };

  // Keep parent_socket open until the ack is sent: the child blocks reading it,
  // so closing it early (on failure) unblocks the child via EOF.
  UniqueFd fuse_fd(recv_result(parent_socket.get()));
  if (!fuse_fd) {
    parent_socket.reset();
    reap();
    return std::nullopt;  // Child's namespace/mount setup failed.
  }

  // The parent's own passthrough root, resolved in the parent's namespace and
  // so unaffected by the child's private mount.
  const int root_fd = ::open(root.c_str(), O_PATH | O_DIRECTORY);
  if (root_fd < 0) {
    parent_socket.reset();
    reap();
    return std::nullopt;
  }
  Tracer tracer(root_fd, root);

  std::string program = "makebelieve-trace";
  std::array<char*, 1> fuse_argv = {program.data()};
  fuse_args args = FUSE_ARGS_INIT(1, fuse_argv.data());
  const std::unique_ptr args_guard = make_scope_ptr(&args, &fuse_opt_free_args);
  fuse_session* se =
      fuse_session_new(&args, &k_ops, sizeof(fuse_lowlevel_ops), &tracer);
  if (se == nullptr) {
    parent_socket.reset();
    reap();
    return std::nullopt;
  }
  const std::unique_ptr session_guard =
      make_scope_ptr(se, &fuse_session_destroy);

  if (fuse_session_custom_io(se, &k_io, fuse_fd.get()) != 0) {
    parent_socket.reset();
    reap();
    return std::nullopt;
  }
  fuse_fd.release();  // Owned by `se` on success.

  std::thread servicing([se] { fuse_session_loop(se); });

  // Tell the child it is now safe to resolve paths through the mount.
  const char ack = 1;
  const ssize_t acked = ::write(parent_socket.get(), &ack, 1);
  static_cast<void>(acked);
  parent_socket.reset();

  Supervision supervision = supervise(pid, out_read.release(), stop);

  // The mount tears down once every process in the namespace has exited, which
  // unblocks the servicing thread. Kill any straggler so that happens even if a
  // descendant outlived the shell.
  ::kill(-pid, SIGKILL);
  fuse_session_exit(se);  // A no-op if the loop already returned.
  servicing.join();

  std::vector<std::filesystem::path> inputs;
  if (!supervision.error && !supervision.canceled) {
    inputs = std::move(tracer).take_read_files();
    std::ranges::sort(inputs);
    inputs.erase(std::ranges::unique(inputs).begin(), inputs.end());
  }
  return to_result(std::move(supervision), std::move(inputs));
}

// Runs @a command without tracing, reporting no inputs. Used when the tracer
// could not be set up.
ProcessUtil::Result run_untraced(const std::filesystem::path& root,
                                 const std::string& command,
                                 const std::stop_token& stop) {
  std::array<int, 2> out_pipe{-1, -1};
  if (::pipe(out_pipe.data()) != 0) {
    return std::unexpected(last_error_code());
  }
  UniqueFd out_read(out_pipe[0]);
  UniqueFd out_write(out_pipe[1]);

  const pid_t pid = ::fork();
  if (pid < 0) {
    return std::unexpected(last_error_code());
  }
  if (pid == 0) {
    out_read.reset();
    if (!prepare_child_io(out_write.get())) {
      _exit(127);
    }
    if (::chdir(root.c_str()) != 0) {
      _exit(127);
    }
    ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);  // Only reached if exec failed.
  }
  out_write.reset();  // Only the child writes.

  return to_result(supervise(pid, out_read.release(), stop), {});
}

}  // namespace

void ProcessUtil::run(const std::filesystem::path& working_directory,
                      const std::string& command,
                      const std::stop_token& stop,
                      Complete on_done) {
  if (stop.stop_requested()) {
    on_done(
        std::unexpected(std::make_error_code(std::errc::operation_canceled)));
    return;
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

  if (std::optional<Result> traced = run_with_tracing(root, command, stop)) {
    on_done(std::move(*traced));
    return;
  }

  // The tracer could not attach; run untraced, reporting no inputs (an empty
  // list is a valid best-effort result).
  on_done(run_untraced(root, command, stop));
}

}  // namespace makebelieve
