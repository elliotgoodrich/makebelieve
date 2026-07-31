/**
 * @file
 * Minimal FUSE example: mounts a single read-only file, "time.txt", whose
 * contents change every second - a random greeting of varying length plus
 * the current UTC time.
 *
 * To make inotify work too, every read() schedules a background thread
 * (see schedule_next_change()) that performs a real write(2) against the
 * mounted file at the next whole-second boundary.
 *
 * Usage:
 * @code
 *   mkdir /tmp/fuse-mnt
 *   fuse_experiment /tmp/fuse-mnt
 *   inotifywait -m /tmp/fuse-mnt/time.txt &
 *   cat /tmp/fuse-mnt/time.txt   # first read kicks off the heartbeat
 *   fusermount3 -u /tmp/fuse-mnt
 * @endcode
 */

#include <fuse.h>
#include <fuse_lowlevel.h>

#include <sys/statvfs.h>

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <string_view>
#include <thread>

namespace {

/// Candidate greetings make_content() picks from at random. These are
/// different lengths to make sure we can't guess the content size, which
/// will be the case if we generated the contents from an arbitrary
/// command.
constexpr std::array<std::string_view, 2> kGreetings = {"hello", "hi"};

/**
 * Absolute path to the mounted file, e.g. "/tmp/fuse-mnt/time.txt". Set
 * once in main() before fuse_main() runs; safe to read from later
 * callbacks or background threads without synchronization - thread
 * creation (both FUSE's worker pool and schedule_dummy_write()'s
 * std::thread) establishes the necessary happens-before edge.
 */
std::string g_mount_file;

/**
 * Debounces schedule_dummy_write(): at most one dummy write is ever
 * pending or in flight, no matter how many reads land while it's
 * outstanding.
 */
std::atomic<bool> g_write_scheduled{false};

/**
 * gettid() of whichever thread is currently inside fire_dummy_write()'s
 * open()/write(), or 0 (never a valid tid) when none is. fuse_context::pid
 * is documented as "Process ID of the calling *thread*". This identifies
 * "our" writer means tracking that specific thread, not comparing against
 * a single fixed pid.
 */
std::atomic<pid_t> g_writer_tid{0};

/**
 * Path to the exposed file.
 */
const std::string_view k_time_path = "/time.txt";

/**
 * Whether the request made by the current thread is the one that
 * `fire_dummy_write()` spawned to perform our dummy write
 */
bool is_self_request() {
  const fuse_context* ctx = fuse_get_context();
  return ctx != nullptr &&
         ctx->pid == g_writer_tid.load(std::memory_order_relaxed);
}

/**
 * Builds time.txt's content for a given moment.
 *
 * @param now The moment to render, typically
 *   std::chrono::system_clock::now().
 * @return The greeting-plus-timestamp content for that moment.
 */
std::string make_content(std::chrono::system_clock::time_point now) {
  const auto seed =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
  const std::size_t index =
      std::uniform_int_distribution<std::size_t>(0, kGreetings.size() - 1)(rng);

  // gmtime_r/strftime are C library calendar/formatting functions and
  // still need a time_t; this is just the boundary conversion for that,
  // not a return to time_t as this file's own representation of "now".
  const std::time_t raw_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&raw_time, &tm);
  std::array<char, 32> time_buf;
  std::strftime(time_buf.data(), time_buf.size(), "%Y-%m-%d %H:%M:%S", &tm);

  std::string content(kGreetings[index]);
  content += ", it is ";
  content += time_buf.data();
  content += " UTC\n";
  return content;
}

/**
 * Performs the actual dummy write against the mounted file, once the
 * delay armed by schedule_next_change() has elapsed. Opening and writing
 * the real mounted path (rather than talking to this process's own state
 * directly) is what makes the write a genuine syscall the kernel routes
 * through the VFS - see the file header comment.
 */
void fire_dummy_write() {
  // Claim the trust window under this thread's own tid before touching the
  // mount, so FUSE::open/FUSE::write (running on a FUSE worker thread, but
  // handling a request whose reported pid is *this* thread's tid) can
  // recognize it as ours.
  g_writer_tid.store(gettid(), std::memory_order_relaxed);
  const int fd = ::open(g_mount_file.c_str(), O_WRONLY);
  if (fd >= 0) {
    constexpr char kDummyByte = '\0';
    ::write(fd, &kDummyByte, sizeof(kDummyByte));
    ::close(fd);
  }
  // Close the trust window before allowing schedule_dummy_write to open a
  // new one (it can't run until g_write_scheduled flips below).
  g_writer_tid.store(0, std::memory_order_relaxed);
  g_write_scheduled.store(false, std::memory_order_relaxed);
}

/**
 * Schedules fire_dummy_write() to run after `delay`, on a new thread,
 * unless one is already pending or in flight (see g_write_scheduled).
 *
 * @param delay How long to wait before performing the dummy write.
 */
void schedule_next_change(std::chrono::milliseconds delay) {
  bool expected = false;
  if (g_write_scheduled.compare_exchange_strong(expected, true)) {
    std::thread([delay] {
      std::this_thread::sleep_for(delay);
      fire_dummy_write();
    }).detach();
  }
}

/// Groups the fuse_operations callbacks as static methods, named after
/// the libfuse operation each one implements (FUSE::getattr is `getattr`,
/// and so on).
struct FUSE {
  /**
   * Reports "/" as a directory and "/time.txt" as a
   * regular file whose size/mtime reflect the content for right now.
   */
  static int getattr(const char* p, struct stat* st, struct fuse_file_info*) {
    const std::string_view path(p);
    std::memset(st, 0, sizeof(*st));
    if (path == "/") {
      st->st_mode = S_IFDIR | 0755;
      st->st_nlink = 2;
      return 0;
    } else if (path == k_time_path) {
      const auto now = std::chrono::system_clock::now();
      st->st_mode = S_IFREG | 0444;
      st->st_nlink = 1;
      // TODO: don't generate the content twice.
      st->st_size = static_cast<off_t>(make_content(now).size());
      st->st_mtime = std::chrono::system_clock::to_time_t(now);
      return 0;
    } else {
      return -ENOENT;
    }
  }

  /**
   * Neither "/" nor "/time.txt" is ever a symlink.
   * EINVAL is what a real filesystem returns from readlink(2) on a
   * non-symlink.
   */
  static int readlink(const char*, char*, size_t) { return -EINVAL; }

  /// Allow opening only "/" as a directory.
  static int opendir(const char* p, struct fuse_file_info*) {
    if (std::string_view(p) != "/") {
      return -ENOTDIR;
    }
    return 0;
  }

  /// Lists "." / ".." / "time.txt" for "/".
  static int readdir(const char* p,
                     void* buf,
                     fuse_fill_dir_t filler,
                     off_t,
                     struct fuse_file_info*,
                     fuse_readdir_flags) {
    const std::string_view path(p);
    if (path != "/") {
      return -ENOENT;
    }
    for (const char* name : {".", "..", "time.txt"}) {
      filler(buf, name, nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    }
    return 0;
  }

  /// No per-directory resource to release.
  static int releasedir(const char*, struct fuse_file_info*) { return 0; }

  /**
   * FUSE `access`: mirrors the read-only-except-our-writer-thread policy
   * enforced by FUSE::open()/FUSE::write().
   */
  static int access(const char* p, int mask) {
    const std::string_view path(p);
    if (path == "/") {
      // Readable/searchable by everyone; there's nowhere to create or
      // remove entries, so never writable.
      return (mask & W_OK) != 0 ? -EACCES : 0;
    } else if (path == k_time_path) {
      return (mask & X_OK) != 0 ? -EACCES : 0;
    } else {
      return -ENOENT;
    }
  }

  /**
   * Read-only for everyone except our own dummy-write thread.
   */
  static int open(const char* path, struct fuse_file_info* fi) {
    if (path != k_time_path) {
      return -ENOENT;
    }
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
      // The file is read-only to everyone except our own dummy-write
      // thread (see fire_dummy_write / is_self_request) - st_mode above
      // advertises 0444 regardless, since that's cosmetic; this check is
      // what actually enforces it.
      return is_self_request() ? 0 : -EACCES;
    }

    // Without direct_io, the kernel can serve a re-read from its page
    // cache instead of calling back into FUSE::read, which would defeat
    // the every-second invalidation above.
    fi->direct_io = 1;
    return 0;
  }

  /**
   * Read our file and fire off a write for when the file would next
   * change based on the time to the next whole second. This means that
   * anyone watching the file will be notified of the change, but after
   * that point we no longer notify anyone unless we are read again.
   */
  static int read(const char* path,
                  char* buf,
                  size_t size,
                  off_t offset,
                  struct fuse_file_info*) {
    if (path != k_time_path) {
      return -ENOENT;
    }
    const auto now = std::chrono::system_clock::now();
    const auto next_second =
        std::chrono::time_point_cast<std::chrono::seconds>(now) +
        std::chrono::seconds(1);

    schedule_next_change(std::chrono::duration_cast<std::chrono::milliseconds>(
        next_second - now));
    const std::string content = make_content(now);
    if (offset < 0 || static_cast<size_t>(offset) >= content.size()) {
      return 0;
    }
    const size_t n =
        std::min(size, content.size() - static_cast<size_t>(offset));
    std::copy_n(content.data() + offset, n, buf);
    return static_cast<int>(n);
  }

  /**
   * Accept only a write from ourselves, which is used to trigger inotify
   * events. The byte is discarded, as the file is read-only and the content is
   * generated on-the-fly.
   */
  static int write(const char* path,
                   const char*,
                   size_t size,
                   off_t,
                   struct fuse_file_info*) {
    if (path != k_time_path) {
      return -ENOENT;
    }
    // Only our own dummy-write thread may write, and even then the byte
    // is discarded: this write exists purely to make the kernel fire
    // inotify, never to change content (see the file header comment and
    // make_content).
    return is_self_request() ? static_cast<int>(size) : -EACCES;
  }

  /**
   * Reports plausible constants; there's no real backing
   * storage to report capacity/usage for.
   */
  static int statfs(const char*, struct statvfs* stbuf) {
    std::memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_namemax = 255;
    // Block/inode counts stay zero: there's no backing storage to report
    // real capacity or usage numbers for.
    return 0;
  }

  /// No-op, we hold no per-open resource.
  static int flush(const char*, struct fuse_file_info*) { return 0; }

  /// No-op, we hold no per-open resource.
  static int release(const char*, struct fuse_file_info*) { return 0; }

  /**
   * FUSE `setxattr`/`getxattr`/`listxattr`/`removexattr`: explicit
   * stand-ins for "extended attributes aren't supported", rather than
   * leaving these four callbacks unset. Functionally identical either
   * way (an unset callback also makes libfuse return -ENOSYS to the
   * kernel), but this way the gap is a visible decision in the source,
   * not just an absence a reader has to notice by what's missing from
   * k_ops. The kernel remaps that -ENOSYS to -EOPNOTSUPP for the
   * calling process (the standard "this filesystem doesn't do xattrs"
   * errno) and stops sending further xattr requests to this mount.
   */
  static int setxattr(const char*, const char*, const char*, size_t, int) {
    return -ENOSYS;
  }

  /// @copydoc FUSE::setxattr
  static int getxattr(const char*, const char*, char*, size_t) {
    return -ENOSYS;
  }

  /// @copydoc FUSE::setxattr
  static int listxattr(const char*, char*, size_t) { return -ENOSYS; }

  /// @copydoc FUSE::setxattr
  static int removexattr(const char*, const char*) { return -ENOSYS; }

  /**
   * Disable FUSE's attribute/entry caches so every
   * stat()/lookup() reaches FUSE::getattr() instead of being answered
   * from a cached value.
   */
  static void* init(struct fuse_conn_info*, struct fuse_config* cfg) {
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    return nullptr;
  }
};

constexpr fuse_operations k_ops = {
    .getattr = FUSE::getattr,
    .readlink = FUSE::readlink,
    .open = FUSE::open,
    .read = FUSE::read,
    .write = FUSE::write,
    .statfs = FUSE::statfs,
    .flush = FUSE::flush,
    .release = FUSE::release,
    .setxattr = FUSE::setxattr,
    .getxattr = FUSE::getxattr,
    .listxattr = FUSE::listxattr,
    .removexattr = FUSE::removexattr,
    .opendir = FUSE::opendir,
    .readdir = FUSE::readdir,
    .releasedir = FUSE::releasedir,
    .init = FUSE::init,
    .access = FUSE::access,
};

}  // namespace

/**
 * Parses just enough of argv to learn the mountpoint, then hands off to
 * fuse_main() for the real command-line handling and event loop.
 */
int main(int argc, char* argv[]) {
  // Parse a throwaway copy of the command line first, purely to learn the
  // mountpoint before it gets passed to fuse_main() below - the mountpoint
  // can land anywhere among argv (e.g. "<mnt> -f" and "-f <mnt>" are both
  // valid), so this can't just be argv[argc - 1]. fuse_parse_cmdline()
  // reallocates its own copy of the arg list rather than touching the
  // caller's argc/argv, so the second, real parse inside
  // fuse_main(argc, argv, ...) still sees the original.
  fuse_args peek_args = FUSE_ARGS_INIT(argc, argv);
  fuse_cmdline_opts opts{};
  if (fuse_parse_cmdline(&peek_args, &opts) != 0 ||
      opts.mountpoint == nullptr) {
    std::fprintf(stderr, "usage: %s [options] <mountpoint>\n", argv[0]);
    return 1;
  }
  g_mount_file = std::string(opts.mountpoint) + std::string(k_time_path);
  std::free(opts.mountpoint);
  fuse_opt_free_args(&peek_args);

  return fuse_main(argc, argv, &k_ops, nullptr);
}
