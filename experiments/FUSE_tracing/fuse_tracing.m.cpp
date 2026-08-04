/**
 * @file
 * Runs a command with a FUSE passthrough filesystem mounted directly on
 * the current directory, and prints every file under the current
 * directory that the command opened for reading.
 *
 * Fork a child into a fresh user+mount namespace, mount a FUSE view
 * directly on top of the current directory (so the mount *replaces* that
 * path rather than living under it), and record every open() the tracer
 * sees.
 *
 * Mounting on the current directory itself, rather than on some ancestor
 * of it, is what keeps parent directories untracked for free: resolving
 * ".." from inside the mount leaves the mount entirely (the kernel just
 * switches back to the real filesystem for the parent), so those lookups
 * never reach the tracer. Only paths that stay under the current
 * directory are ever seen.
 *
 * Usage:
 * @code
 *   fuse_tracing_experiment <command> [args...]
 * @endcode
 */

#include <fuse_lowlevel.h>

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// --- Child <-> parent fuse_fd handoff over a socketpair, via SCM_RIGHTS ---
//
// The /dev/fuse fd used in the mount(2) call must be opened *inside* the
// child's new mount namespace, after unshare() - a pre-fork-opened fd
// inherited via the normal fork fd-table copy makes mount(2) fail with
// EINVAL.  That means the parent (which keeps running and must service the
// mount) can't get its own copy "for free" the way it could if the fd had
// existed before fork - it must receive the child's fd instead, after the child
// opens and mounts it. This one-byte-payload protocol doubles as a
// success/failure signal: a payload of kHandoffOk always carries the fd as
// SCM_RIGHTS ancillary data; kHandoffFailed means the child hit a setup
// error before reaching the mount, and no fd follows.
constexpr char kHandoffOk = 1;
constexpr char kHandoffFailed = 0;

// sock and fd below are both plain POSIX fds; there's no stronger type to
// give them here.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void send_result(int sock, int fd) {
  char payload = fd >= 0 ? kHandoffOk : kHandoffFailed;
  struct iovec iov {
    .iov_base = &payload, .iov_len = 1,
  };
  struct msghdr msg {
    .msg_iov = &iov, .msg_iovlen = 1,
  };

  std::array<char, CMSG_SPACE(sizeof(int))> cmsgbuf{};
  if (fd >= 0) {
    msg.msg_control = cmsgbuf.data();
    msg.msg_controllen = cmsgbuf.size();
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
  }
  // Best-effort: the child is about to _exit() or execve() regardless of
  // whether this send actually lands.
  ::sendmsg(sock, &msg, 0);
}

// Returns the received fd on success, or -1 if the child reported failure
// or the handoff protocol was otherwise violated.
int recv_result(int sock) {
  char payload = 0;
  struct iovec iov {
    .iov_base = &payload, .iov_len = 1,
  };
  std::array<char, CMSG_SPACE(sizeof(int))> cmsgbuf{};
  struct msghdr msg {
    .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cmsgbuf.data(),
    .msg_controllen = cmsgbuf.size(),
  };

  if (::recvmsg(sock, &msg, 0) <= 0 || payload != kHandoffOk) {
    return -1;
  }
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == nullptr || cmsg->cmsg_type != SCM_RIGHTS) {
    return -1;
  }
  int fd = -1;
  std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
  return fd;
}

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

// Everything below runs only in the forked child, which either execve()s
// the traced command or _exit()s - it never returns to main(). Prints a
// diagnostic to stderr before exiting: sandbox setup can fail for
// environment reasons (e.g. unprivileged user namespaces disabled) that
// are otherwise silent and confusing to debug.
[[noreturn]] void fail_child(int handoff_sock, const char* what) {
  std::fprintf(stderr,
               "fuse_tracing_experiment: tracer setup failed at %s: %s\n", what,
               std::strerror(errno));
  send_result(handoff_sock, -1);
  _exit(125);
}

/// @pre argv.back() == nullptr, i.e. argv is already null-terminated
[[noreturn]] void run_child(
    std::span<char*> argv,
    const std::filesystem::path& cwd,
    uid_t real_uid,
    // gid_t and the handoff socket fd below are
    // both small integer types; there's no
    // stronger type to give either one here.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    gid_t real_gid,
    int handoff_sock) {
  assert(argv.back() == nullptr);
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    fail_child(handoff_sock, "unshare");
  }
  if (!write_whole_file("/proc/self/setgroups", "deny")) {
    fail_child(handoff_sock, "setgroups");
  }
  if (!write_whole_file("/proc/self/uid_map",
                        "0 " + std::to_string(real_uid) + " 1")) {
    fail_child(handoff_sock, "uid_map");
  }
  if (!write_whole_file("/proc/self/gid_map",
                        "0 " + std::to_string(real_gid) + " 1")) {
    fail_child(handoff_sock, "gid_map");
  }
  // Detach mount propagation so nothing here leaks to/from the host.
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    fail_child(handoff_sock, "mount MS_PRIVATE");
  }

  // Opened fresh, here, post-unshare - a pre-fork-opened /dev/fuse fd
  // inherited from the parent fails the mount(2) call below with EINVAL
  // (see the handoff protocol comment above).
  const int fuse_fd = ::open("/dev/fuse", O_RDWR);
  if (fuse_fd < 0) {
    fail_child(handoff_sock, "open /dev/fuse");
  }

  std::array<char, 256> mount_data{};
  std::snprintf(mount_data.data(), mount_data.size(),
                "fd=%d,rootmode=040000,user_id=0,group_id=0", fuse_fd);
  if (::mount("fuse-tracing", cwd.c_str(), "fuse", MS_NOSUID | MS_NODEV,
              mount_data.data()) != 0) {
    fail_child(handoff_sock, "mount fuse");
  }

  // Hand the fd off to the parent *now*, before doing anything else that
  // requires resolving a path through the just-created FUSE mount (the
  // chdir() below does exactly that). Until the parent has the fd and is
  // actively servicing it, nothing will ever reply to that lookup,
  // deadlocking this process forever.
  send_result(handoff_sock, fuse_fd);
  ::close(fuse_fd);

  // Block until the parent acks that its servicing thread is up and
  // reading the mount - only then is it safe to resolve a path through it.
  char ack = 0;
  if (::read(handoff_sock, &ack, 1) != 1) {
    fail_child(handoff_sock, "waiting for parent ack");
  }
  ::close(handoff_sock);

  ::chdir(cwd.c_str());
  ::execvp(argv[0], argv.data());
  _exit(127);  // execve failed
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

// One O_PATH-fd-backed inode in the tracer's passthrough view, keyed by a
// synthetic fuse_ino_t (root is always FUSE_ROOT_ID). Owns its fd via
// UniqueFd, which is what lets Tracer itself go without a custom
// destructor - destroying m_inodes closes every fd for free.
struct Inode {
  UniqueFd fd;
  std::filesystem::path real_path;
};

// Services the traced command's FUSE mount: a read-only passthrough view
// over `real_path`s rooted at the current directory, recording the real
// path of every file actually opened.
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
      // fuse_reply_err reads errno before fd's destructor (which may
      // itself touch errno via close()) runs at this return.
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

    const struct fuse_entry_param e = {
        .ino = it->second,
        .attr = st,
        .attr_timeout = 0,
        .entry_timeout = 0,
    };
    fuse_reply_entry(req, &e);
  }

  void getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*) {
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

  void open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
    Inode* n = find(ino);
    if (n == nullptr) {
      fuse_reply_err(req, EIO);
      return;
    }
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
      fuse_reply_err(req, EROFS);
      return;
    }
    // Reopening n->fd (an O_PATH fd) via "." only works for directories -
    // for a regular file that fails with ENOTDIR. The /proc/self/fd/N
    // route reopens either kind uniformly (the same trick libfuse's own
    // passthrough_ll.c reference uses).
    std::array<char, 32> proc_fd_path{};
    std::snprintf(proc_fd_path.data(), proc_fd_path.size(), "/proc/self/fd/%d",
                  n->fd.get());
    UniqueFd fd(::open(proc_fd_path.data(), O_RDONLY));
    if (!fd) {
      fuse_reply_err(req, errno);
      return;
    }

    {
      // Record the file path read.
      const std::scoped_lock lock(m_mutex);
      m_read_files.push_back(n->real_path);
    }

    // Ownership passes to libfuse's opaque per-open handle (fi->fh) from
    // here on - release() gives up the fd without closing it; Tracer::
    // release() below is what actually closes it once FUSE is done.
    fi->fh = static_cast<std::uint64_t>(fd.release());
    fi->direct_io = 1;
    fuse_reply_open(req, fi);
  }

  void read(fuse_req_t req,
            fuse_ino_t /*ino*/,
            size_t size,
            off_t off,
            struct fuse_file_info* fi) {
    std::vector<char> buf(size);
    const ssize_t n = ::pread(static_cast<int>(fi->fh), buf.data(), size, off);
    if (n < 0) {
      fuse_reply_err(req, errno);
      return;
    }
    fuse_reply_buf(req, buf.data(), static_cast<std::size_t>(n));
  }

  void release(fuse_req_t req, fuse_ino_t /*ino*/, struct fuse_file_info* fi) {
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

// Turns a Tracer member function into a plain C fuse_lowlevel_ops callback
// via fuse_req_userdata.
template <auto MemFn>
struct Trampoline;

template <typename... Args, void (Tracer::*MemFn)(fuse_req_t, Args...)>
struct Trampoline<MemFn> {
  static auto call(fuse_req_t req, Args... args) {
    try {
      auto* tracer = static_cast<Tracer*>(fuse_req_userdata(req));
      return (tracer->*MemFn)(req, args...);
    } catch (...) {
      fuse_reply_err(req, EIO);
    }
  }
};

template <auto MemFn>
inline constexpr auto trampoline = Trampoline<MemFn>::call;

constexpr struct fuse_lowlevel_ops kOps = {
    .lookup = trampoline<&Tracer::lookup>,
    .getattr = trampoline<&Tracer::getattr>,
    .open = trampoline<&Tracer::open>,
    .read = trampoline<&Tracer::read>,
    .release = trampoline<&Tracer::release>,
};

ssize_t custom_io_read(int fd, void* buf, size_t buf_len, void* /*userdata*/) {
  return ::read(fd, buf, buf_len);
}

ssize_t custom_io_writev(int fd,
                         struct iovec* iov,
                         int count,
                         void* /*userdata*/) {
  return ::writev(fd, iov, count);
}

constexpr struct fuse_custom_io kIo = {
    .writev = custom_io_writev,
    .read = custom_io_read,
    .splice_receive = nullptr,
    .splice_send = nullptr,
};

template <typename T, typename D>
std::unique_ptr<T, D> make_scope_ptr(T* p, D deleter) {
  return std::unique_ptr<T, D>(p, deleter);
}

/// Forks, mounts a tracer FUSE filesystem directly on `cwd`, runs `argv`
/// inside it, and returns the real paths of every file it opened for
/// reading. On any setup failure, prints a diagnostic to stderr and
/// returns std::nullopt.
///
/// @param argv Command and arguments to exec, terminated by a trailing
/// nullptr - i.e. argv.back() == nullptr, exactly like main()'s own
/// argv/argc (where argv[argc] == nullptr is guaranteed) - so it can be
/// passed to execvp() as-is, with no separate null-terminated copy built
/// along the way.
///
/// @pre argv.back() == nullptr, i.e. argv is already null-terminated
std::optional<std::pair<int, std::vector<std::filesystem::path>>> run_traced(
    std::span<char*> argv,
    const std::filesystem::path& cwd) {
  assert(argv.back() == nullptr);

  std::array<int, 2> sv{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) != 0) {
    std::perror("fuse_tracing_experiment: socketpair");
    return std::nullopt;
  }
  UniqueFd parent_socket(sv[0]);
  UniqueFd child_socket(sv[1]);

  const uid_t real_uid = ::getuid();
  const gid_t real_gid = ::getgid();

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("fuse_tracing_experiment: fork");
    return std::nullopt;
  }

  if (pid == 0) {
    // If we're the child process.
    parent_socket.reset();
    run_child(argv, cwd, real_uid, real_gid, child_socket.release());
  }

  // Otherwise we are the parent process.
  child_socket.reset();

  // parent_socket is deliberately NOT closed as soon as the fd is
  // received: the child blocks on a read() of this same socket, waiting
  // for an ack that it's safe to proceed (see run_child()) - it must stay
  // open, one way or another, until either that ack is sent or we give
  // up (closing it unacked also unblocks the child, via EOF, into its
  // own failure path).
  UniqueFd fuse_fd(recv_result(parent_socket.get()));
  if (!fuse_fd) {
    // Child's namespace/mount setup failed before it could exec anything;
    // still must reap it to avoid a zombie.
    int status = 0;
    ::waitpid(pid, &status, 0);
    return std::nullopt;
  }

  // The parent's own passthrough root - independent of the child's private
  // mount namespace, which is invisible outside that namespace, so this
  // still resolves to the real, unmounted directory.
  const int root_fd = ::open(cwd.c_str(), O_PATH | O_DIRECTORY);
  if (root_fd < 0) {
    std::perror("fuse_tracing_experiment: open cwd");
    int status = 0;
    ::waitpid(pid, &status, 0);
    return std::nullopt;
  }

  Tracer tracer(root_fd, cwd);

  std::string prog_name = "fuse-tracing-experiment";
  std::array<char*, 1> fuse_argv = {prog_name.data()};
  struct fuse_args args = FUSE_ARGS_INIT(1, fuse_argv.data());
  const std::unique_ptr args_guard = make_scope_ptr(&args, &fuse_opt_free_args);
  struct fuse_session* se =
      fuse_session_new(&args, &kOps, sizeof(struct fuse_lowlevel_ops), &tracer);
  if (se == nullptr) {
    int status = 0;
    ::waitpid(pid, &status, 0);
    return std::nullopt;
  }
  const std::unique_ptr session_guard =
      make_scope_ptr(se, &fuse_session_destroy);

  if (fuse_session_custom_io(se, &kIo, fuse_fd.get()) != 0) {
    int status = 0;
    ::waitpid(pid, &status, 0);
    return std::nullopt;
  }
  fuse_fd.release();  // owned by `se` only on success

  std::thread servicing([se] { fuse_session_loop(se); });

  // Tell the child it's now safe to resolve paths through the mount (e.g.
  // its chdir()) - only from this point on is anything actually reading
  // the mount and able to reply.
  const char ack = 1;
  ::write(parent_socket.get(), &ack, 1);
  parent_socket.reset();

  int status = 0;
  ::waitpid(pid, &status, 0);

  // Once the child (the last member of its mount namespace) has exited,
  // the namespace and its mount are torn down by the kernel, which
  // unblocks the servicing thread's read() - no hang/timeout logic
  // needed. fuse_session_exit() is a defensive no-op if the loop has
  // already returned on its own by this point.
  fuse_session_exit(se);
  servicing.join();

  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return std::pair(exit_code, std::move(tracer).take_read_files());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <command> [args...]\n", argv[0]);
    return 2;
  }

  // Copies pointer values only (not the strings themselves) - argv[i] is
  // already char*, same as main()'s own argv, so no const_cast is ever
  // needed downstream when this reaches execvp().
  //
  // Goes up to argv + argc + 1, one past the real arguments, to carry
  // along argv[argc] itself - the C/POSIX-guaranteed trailing nullptr -
  // so run_traced()/run_child() receive an already null-terminated span
  // and don't need to build their own null-terminated copy before exec'ing.
  std::vector<char*> command(argv + 1, argv + argc + 1);
  const std::filesystem::path cwd = std::filesystem::current_path();

  auto result = run_traced(command, cwd);
  if (!result.has_value()) {
    std::fprintf(stderr, "fuse_tracing_experiment: failed to run command\n");
    return 1;
  }
  auto& [exit_code, read_files] = *result;
  std::ranges::sort(read_files);
  read_files.erase(std::ranges::unique(read_files).begin(), read_files.end());

  std::cout << "Files read from " << cwd.string() << ":\n";
  for (const std::filesystem::path& p : read_files) {
    std::cout << "  " << p.string() << '\n';
  }

  return exit_code;
}
