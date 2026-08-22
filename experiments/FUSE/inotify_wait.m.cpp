/**
 * @file
 * Watches a path for inotify IN_MODIFY events over a fixed window,
 * printing one "EVENT" line per event observed as it happens (flushed
 * immediately, not buffered until exit). Exists so smoke_test.sh can
 * verify time.txt's inotify heartbeat (see fuse.m.cpp) without depending
 * on inotify-tools being installed - this only needs the same
 * POSIX/Linux headers fuse_experiment already does.
 *
 * Run in the background for the duration of a whole test scenario with
 * its stdout appended to the same log file the test's own reads are
 * recorded into (see smoke_test.sh), rather than as a one-shot "did an
 * event happen" check - that lets the test assert not just that a
 * notification arrived, but how many arrived by which point in the
 * scenario.
 *
 * Usage: inotify_wait <path> <duration_seconds>
 * @return 0 once the duration elapses, 2 on setup error.
 */

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <path> <duration_seconds>\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  const int duration_s = std::atoi(argv[2]);

  const int fd = inotify_init1(IN_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "inotify_init1: %s\n", std::strerror(errno));
    return 2;
  }
  if (inotify_add_watch(fd, path, IN_MODIFY) < 0) {
    std::fprintf(stderr, "inotify_add_watch(%s): %s\n", path,
                 std::strerror(errno));
    return 2;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
  while (true) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
            .count();
    if (remaining_ms <= 0) {
      break;
    }

    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int rc = poll(&pfd, 1, static_cast<int>(remaining_ms));
    if (rc < 0) {
      std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
      return 2;
    }
    if (rc == 0) {
      break;  // Deadline reached.
    }

    // One read() can return several coalesced inotify_event structs back
    // to back, so walk the whole buffer rather than assuming just one.
    alignas(struct inotify_event) std::array<char, 4096> buf;
    const ssize_t n = read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(n)) {
      const auto* event =
          reinterpret_cast<const struct inotify_event*>(buf.data() + offset);
      std::printf("EVENT\n");
      std::fflush(stdout);
      offset += sizeof(struct inotify_event) + event->len;
    }
  }
  return 0;
}
