// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {errno, std::system_category()};
}

}  // namespace

void ProcessUtil::run(const std::filesystem::path& working_directory,
                      const std::string& command,
                      const std::stop_token& stop,
                      Complete on_done) {
  // A pipe carries the child's standard output back to us.
  std::array<int, 2> pipe_fds{-1, -1};
  if (pipe(pipe_fds.data()) != 0) {
    on_done(std::unexpected(last_error_code()));
    return;
  }
  const int read_fd = pipe_fds[0];
  const int write_fd = pipe_fds[1];

  const pid_t pid = fork();
  if (pid < 0) {
    const std::error_code ec = last_error_code();
    close(read_fd);
    close(write_fd);
    on_done(std::unexpected(ec));
    return;
  }
  if (pid == 0) {
    // In the child: only async-signal-safe calls until exec. Send stdout down
    // the pipe, point the working directory at the source so the command's
    // relative paths resolve there, then hand the command line to the shell.
    close(read_fd);
    dup2(write_fd, STDOUT_FILENO);
    close(write_fd);
    if (chdir(working_directory.c_str()) != 0) {
      _exit(127);
    }
    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);  // Only reached if exec failed.
  }

  close(write_fd);  // Only the child writes.
  // Read without blocking so a grandchild that inherited the pipe cannot keep
  // us waiting once our direct child has exited.
  fcntl(read_fd, F_SETFL, fcntl(read_fd, F_GETFL) | O_NONBLOCK);

  // Terminate the child if a stop is requested while we wait. The callback runs
  // synchronously from the constructor if @a stop is already requested.
  // TODO: this signals the shell, not a tree of grandchildren it may spawn; a
  // process group would be needed to reach those.
  const std::stop_callback on_stop(stop, [pid] { kill(pid, SIGKILL); });

  std::string output;
  std::array<char, 4096> buffer{};
  const auto drain = [&] {
    while (true) {
      const ssize_t count = read(read_fd, buffer.data(), buffer.size());
      if (count <= 0) {
        return;  // No data available right now, or end of file.
      }
      output.append(buffer.data(), static_cast<std::size_t>(count));
    }
  };

  // Drain the pipe until the direct child exits. Waiting on the child rather
  // than on pipe end-of-file is what keeps an inherited-pipe grandchild from
  // wedging us, and draining as we go keeps a full pipe from blocking the
  // child's writes.
  std::error_code wait_error;
  while (true) {
    pollfd descriptor{.fd = read_fd, .events = POLLIN, .revents = 0};
    poll(&descriptor, 1, 100);  // Wake on data or hangup, or re-check at 100ms.
    drain();

    int status = 0;
    const pid_t reaped = waitpid(pid, &status, WNOHANG);
    if (reaped == pid) {
      break;
    }
    if (reaped < 0 && errno != EINTR) {
      wait_error = last_error_code();
      break;
    }
  }
  drain();  // Whatever the child buffered just before it exited.
  close(read_fd);

  if (wait_error) {
    on_done(std::unexpected(wait_error));
  } else if (stop.stop_requested()) {
    on_done(
        std::unexpected(std::make_error_code(std::errc::operation_canceled)));
  } else {
    // Inputs are left empty: the Linux launch does not trace reads yet (a
    // FUSE-based tracer, mirroring experiments/FUSE_tracing, comes later).
    on_done(Output{.standard_output = std::move(output), .inputs = {}});
  }
}

}  // namespace makebelieve
