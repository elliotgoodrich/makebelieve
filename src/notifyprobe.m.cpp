// SPDX-License-Identifier: MIT
/**
 * @file
 * Arms an inotify watch on a directory, runs a trigger command, then waits for
 * an IN_MODIFY notification naming a given file.
 *
 * usage: notifyprobe <dir> <name> <timeout_ms> -- <trigger> [args...]
 * exit:  0 notification seen, 1 timed out, 2 usage or setup error.
 */

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <poll.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Runs @a argv to completion; returns false if it could not be launched.
bool run_trigger(char** argv) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    ::execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6 || std::string_view(argv[4]) != "--") {
    std::fprintf(stderr,
                 "usage: %s <dir> <name> <timeout_ms> -- <trigger> [args...]\n",
                 argv[0]);
    return 2;
  }
  const char* directory = argv[1];
  const std::string_view target = argv[2];
  const int timeout_ms = std::atoi(argv[3]);
  char** trigger = argv + 5;

  const int fd = ::inotify_init1(IN_CLOEXEC);
  if (fd < 0) {
    std::perror("notifyprobe: inotify_init1");
    return 2;
  }
  // IN_MODIFY only, so a read trigger's IN_ACCESS does not count.
  if (::inotify_add_watch(fd, directory, IN_MODIFY) < 0) {
    std::perror("notifyprobe: inotify_add_watch");
    ::close(fd);
    return 2;
  }

  if (!run_trigger(trigger)) {
    std::perror("notifyprobe: fork");
    ::close(fd);
    return 2;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  alignas(inotify_event) std::array<char, 4096> buffer{};
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      return 1;
    }
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready =
        ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("notifyprobe: poll");
      return 2;
    }
    if (ready == 0) {
      return 1;
    }

    const ssize_t count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("notifyprobe: read");
      return 2;
    }
    for (ssize_t offset = 0; offset < count;) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      const auto* event =
          reinterpret_cast<const inotify_event*>(buffer.data() + offset);
      if (event->len > 0 && target == event->name) {
        return 0;
      }
      offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
    }
  }
}
