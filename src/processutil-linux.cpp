// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include <exec/task.hpp>
#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
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

#include <fuse_lowlevel.h>

#include <fcntl.h>
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

  if (::recvmsg(sock, &msg, 0) != static_cast<ssize_t>(sizeof(payload))) {
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

  // Set when a request could not be handled, which may have lost a read.
  std::atomic<bool> m_failed = false;
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

  // A fresh user + mount namespace, so the mount below is invisible elsewhere
  // and can be made without real privilege.
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0 ||
      !write_whole_file("/proc/self/setgroups", "deny") ||
      !write_whole_file("/proc/self/uid_map", uid_map) ||
      !write_whole_file("/proc/self/gid_map", gid_map) ||
      // Detach mount propagation so nothing here leaks to or from the host.
      ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    fail(errno);
  }

  // Opened fresh, post-unshare - a pre-fork-opened /dev/fuse fd inherited from
  // the parent fails the mount(2) below with EINVAL.
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

  if (::chdir(cwd.c_str()) != 0) {
    _exit(127);
  }
  ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
  _exit(127);  // Only reached if exec failed.
}

// A running command, leading its own process group. terminate() kills that
// group and waits for the command itself to exit, through the IoContext,
// before reaping it. Destroying one that has not been reaped does the same,
// blocking, as a last resort. Whatever else is in the group is only asked to
// exit: those processes are not our children, so their exit is neither
// waited for nor reaped, and anything that has left the group is not killed.
class ChildProcess {
  pid_t m_pid;

  // Readable once the command has exited; invalid if the kernel could not
  // open it.
  UniqueFd m_exited;

  bool m_reaped = false;

 public:
  explicit ChildProcess(pid_t pid)
      : m_pid(pid),
        m_exited(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))) {}

  ~ChildProcess() {
    if (!m_reaped) {
      kill();
      reap();
    }
  }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&&) = delete;
  ChildProcess& operator=(ChildProcess&&) = delete;

  // Readable once the command has exited; -1 if the kernel could not say.
  [[nodiscard]] int exited() const noexcept { return m_exited.get(); }

  // Kills the command and whatever is left in its process group - requested,
  // not confirmed, for the latter. Still safe once the command has been
  // reaped, when only what it left behind is signalled.
  void kill() const noexcept {
    ::kill(-m_pid, SIGKILL);
    if (!m_reaped) {
      ::kill(m_pid, SIGKILL);
    }
  }

  // Waits for the command, which has exited or been killed.
  void reap() noexcept {
    int status = 0;
    while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
    }
    m_reaped = true;
  }

  // Kills the command, and what is left in its group, then waits for it to
  // exit through @a io and reaps it - which then no longer blocks.
  exec::task<void> terminate(IoContext& io) {
    if (m_reaped) {
      co_return;
    }
    kill();
    if (m_exited) {
      co_await (stdexec::write_env(io.async_wait(m_exited.get()),
                                   stdexec::prop{stdexec::get_stop_token,
                                                 stdexec::never_stop_token{}}) |
                stdexec::upon_error([](std::error_code) noexcept {}));
    }
    reap();
  }
};

// Kills @a child on stop.
struct Kill {
  const ChildProcess* child;

  void operator()() const noexcept { child->kill(); }
};

// What supervising a running command produced: its stdout, plus how it ended.
struct Supervision {
  std::string output;
  std::error_code error;  // set if waiting on the child failed
  bool canceled =
      false;  // true if it was terminated because stop was requested
};

// Collects @a child's stdout from @a read_fd until it exits, then reaps it,
// killing it if @a stop is requested. Waits on both through @a io. Knows
// nothing of tracing.
exec::task<Supervision> supervise(IoContext& io,
                                  ChildProcess& child,
                                  UniqueFd read_fd,
                                  stdexec::inplace_stop_token stop) {
  Supervision result;
  if (child.exited() < 0) {
    result.error = last_error_code();
    co_return result;  // Killed and reaped by its owner.
  }

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
  // pipe from wedging us, and reading as it arrives keeps a full pipe from
  // blocking the command.
  std::error_code wait_error;
  bool open = true;
  {
    // Gone before the child is reaped, so a stop can never kill a process
    // that has reused its id.
    const stdexec::inplace_stop_callback<Kill> on_stop(stop, Kill{&child});
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
      child.kill();
    }
  }

  child.reap();
  if (open) {
    drain();  // Whatever the child wrote just before it exited.
  }

  result.error = wait_error;
  result.canceled = !wait_error && stop.stop_requested();
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
                             .inputs = std::move(inputs)};
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

  // Why the session stopped serving before it was ended, if it did.
  std::error_code m_error;

  // ProcessUtilTestUtil's tracing_service, due at the first request.
  std::optional<std::error_code> m_forced_error;

  TracingSession(int root_fd, std::filesystem::path root, int descriptor)
      : m_tracer(root_fd, std::move(root)), m_descriptor(descriptor) {}

  // Handles every request waiting; false once the session has ended.
  bool process() noexcept {
    if (m_forced_error.has_value()) {
      m_error = *m_forced_error;
      m_forced_error.reset();
      return false;
    }
    while (!fuse_session_exited(m_session)) {
      const int received = fuse_session_receive_buf(m_session, &m_buffer);
      if (received == -EAGAIN) {
        return true;
      }
      if (received == -EINTR) {
        continue;
      }
      if (received < 0) {
        // Requests may have gone unanswered, so reads may have gone
        // unrecorded. (The mount going away with the command reads as the
        // session ending, not as an error.)
        m_error = std::error_code(-received, std::system_category());
        return false;
      }
      if (received == 0) {
        return false;
      }
      fuse_session_process_buf(m_session, &m_buffer);
    }
    return false;
  }

 public:
  // Serves the mount behind @a fuse_fd as a passthrough onto @a root, or says
  // why it cannot.
  static std::expected<std::unique_ptr<TracingSession>, std::error_code> open(
      const std::filesystem::path& root,
      UniqueFd fuse_fd) {
    // The parent's own passthrough root, resolved in the parent's namespace
    // and so unaffected by the child's private mount.
    const int root_fd = ::open(root.c_str(), O_PATH | O_DIRECTORY);
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
  // or the session ends. Answering one takes real filesystem calls, which slow
  // storage can stretch out, so they run where this task resumes - a worker -
  // rather than on the IoContext's thread, where they would hold up every
  // other wait. One loop per session, so its requests are served one batch at
  // a time.
  exec::task<void> serve(IoContext& io, stdexec::inplace_stop_token stop) {
    while (true) {
      const bool ready = co_await (
          stdexec::write_env(io.async_wait(m_descriptor),
                             stdexec::prop{stdexec::get_stop_token, stop}) |
          stdexec::then([]() noexcept { return true; }) |
          stdexec::upon_error([](std::error_code) noexcept { return false; }) |
          stdexec::upon_stopped([]() noexcept { return false; }));
      if (!ready || !process()) {
        co_return;
      }
    }
  }

  // Ends the session, so that serving it ends too.
  void end() noexcept { fuse_session_exit(m_session); }

  // Whether serving stopped because tracing failed, rather than because the
  // session was ended.
  [[nodiscard]] bool failed() const noexcept {
    return static_cast<bool>(m_error);
  }

  // Makes serving fail with @a error at the first request. For
  // ProcessUtilTestUtil.
  void fail_at_first_request(std::error_code error) noexcept {
    m_forced_error = error;
  }

  // The files the command opened for reading, sorted and without repeats -
  // or why tracing failed while it ran, in which case what was recorded may be
  // incomplete and is not reported at all.
  [[nodiscard]] std::expected<std::vector<std::filesystem::path>,
                              std::error_code>
  take_inputs() && {
    if (m_error) {
      return std::unexpected(m_error);
    }
    std::expected<std::vector<std::filesystem::path>, std::error_code> inputs =
        std::move(m_tracer).take_read_files();
    if (inputs.has_value()) {
      std::ranges::sort(*inputs);
      inputs->erase(std::ranges::unique(*inputs).begin(), inputs->end());
    }
    return inputs;
  }
};

// Supervises @a child, then ends its tracing @a session: kills anything left
// in its process group, so the mount tears down even if a descendant outlived
// the shell, and stops @a stop_serving. It ends the session however
// supervising went, so that serving always ends too.
exec::task<Supervision> supervise_then_end(
    IoContext& io,
    ChildProcess& child,
    UniqueFd read_fd,
    stdexec::inplace_stop_token stop,
    TracingSession& session,
    stdexec::inplace_stop_source& stop_serving) {
  Supervision supervision;
  try {
    supervision = co_await supervise(io, child, std::move(read_fd), stop);
  } catch (...) {
    supervision.error = std::make_error_code(std::errc::not_enough_memory);
  }
  child.kill();
  session.end();
  stop_serving.request_stop();
  co_return supervision;
}

// Serves @a session until it is stopped through @a stop or ends, and kills
// @a child if serving stopped because tracing failed: a command whose mount
// goes unserved would block on it forever, and its build has failed anyway.
exec::task<void> serve_or_kill(IoContext& io,
                               TracingSession& session,
                               ChildProcess& child,
                               stdexec::inplace_stop_token stop) {
  co_await session.serve(io, stop);
  if (session.failed()) {
    child.kill();
  }
}

// Setting up tracing, which spans the command's launch, since the FUSE device
// has to be opened inside the command's own namespace:
//
// 1. prepare(), before the launch: a socket pair for the child to hand the
//    device back over, and its uid and gid maps, formatted now because the
//    child may not allocate.
// 2. run_child(), in the child: enters a fresh namespace, mounts the device
//    over the root, hands it back and waits.
// 3. attach(), in the parent: serves the device it was handed, or reports why
//    the child could not set up; then release() tells the child the session is
//    ready, and only then does the command run.
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
                                           stdexec::inplace_stop_token stop) {
  std::array<int, 2> out_pipe{-1, -1};
  if (::pipe(out_pipe.data()) != 0) {
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
      ProcessUtilTestUtil::take(
          static_cast<int>(ProcessUtilTestUtil::Failure::tracing_setup))
          .value_or(std::error_code{})
          .value();

  const pid_t pid = ::fork();
  if (pid < 0) {
    co_return std::unexpected(last_error_code());
  }
  if (pid == 0) {
    out_read.reset();
    setup->run_child(root, command, out_write.get(), forced_setup_error);
  }
  ChildProcess child(pid);
  setup->launched();
  out_write.reset();  // Only the child writes stdout.

  std::expected<std::unique_ptr<TracingSession>, std::error_code> session =
      setup->attach(root);
  if (!session.has_value()) {
    co_await child.terminate(io);
    co_return std::unexpected(session.error());
  }
  if (const std::optional<std::error_code> forced = ProcessUtilTestUtil::take(
          static_cast<int>(ProcessUtilTestUtil::Failure::tracing_service))) {
    (*session)->fail_at_first_request(*forced);
  }
  setup->release();

  // Supervise the command and serve its requests side by side. Serving has
  // ended, and so is done with the session, by the time both have completed.
  stdexec::inplace_stop_source stop_serving;
  Supervision supervision = co_await stdexec::when_all(
      supervise_then_end(io, child, std::move(out_read), stop, **session,
                         stop_serving),
      serve_or_kill(io, **session, child, stop_serving.get_token()));
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
  co_return co_await run_traced(io, std::move(root), std::move(command), stop);
}

namespace {

// What `/proc/<id>/status` says about @a field, or nothing when the file or
// the field is missing - which is what a process that exited between the
// request and this lookup gives.
std::optional<std::string> read_proc_status(std::uint32_t id,
                                            std::string_view field) {
  std::ifstream status("/proc/" + std::to_string(id) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    if (!line.starts_with(field)) {
      continue;
    }
    const std::size_t start = line.find_first_not_of(" \t", field.size());
    if (start != std::string::npos) {
      return line.substr(start);
    }
  }
  return std::nullopt;
}

}  // namespace

std::uint32_t ProcessUtil::self() {
  return static_cast<std::uint32_t>(::getpid());
}

std::string ProcessUtil::name_of(std::uint32_t pid) {
  return read_proc_status(pid, "Name:").value_or("unknown");
}

}  // namespace makebelieve
