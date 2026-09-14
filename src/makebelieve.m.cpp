// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"
#include "consoleinterrupthandler.hpp"
#include "filesystemutil.hpp"
#include "realdirectorytree.hpp"
#include "unmountchannel.hpp"
#include "virtualfilesystem.hpp"
#include "workqueue.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace makebelieve;

int usage(const char* program) {
  std::println(stderr, "usage: {} mount|unmount <mountpoint>", program);
  return 1;
}

// Blocks until any of @a tokens has a stop requested, returning at once if one
// is already stopped. One semaphore permit per token, so however many callbacks
// fire, the count reaches at most sizeof...(Tokens) - the semaphore's capacity.
template <std::same_as<std::stop_token>... Tokens>
void block_until_any(const Tokens&... tokens) {
  static_assert(sizeof...(Tokens) > 0, "block_until_any needs a token");
  std::counting_semaphore<sizeof...(Tokens)> stopped{0};
  auto on_stop = [&stopped] { stopped.release(); };
  const std::array callbacks{std::stop_callback{tokens, on_stop}...};
  stopped.acquire();
}

  /// Returns a `CommandRunner` that hands each command to @a queue and returns
  /// at once, rather than running it on the calling thread - so the read that
  /// triggered a build gets its placeholder back immediately instead of
  /// blocking until @a runner finishes.
  ///
  /// A completion must not outlive this tree, so @a queue has to be joined
  /// while the tree is still alive: declare the queue before the tree and a
  /// `WorkQueue::JoinGuard` after it.
  ///
  /// Work the queue never started is run with an already-stopped token, which
  /// @a runner must answer without starting the command - `shell_runner` does,
  /// since `ProcessUtil` reports cancellation without spawning.
  /// @pre @a queue outlives this tree's use of the returned runner.
BuildDirectoryTree::CommandRunner queued_runner(
    WorkQueue& queue,
    CommandRunner runner) {
  return [&queue, runner = std::move(runner)](
             std::string command, std::stop_token stop, BuildComplete on_done) {
    queue.submit(
        [runner, command = std::move(command),
         on_done = std::move(on_done)](const std::stop_token& cancel) mutable {
          // An already-stopped token means the queue never reached this
          // command and is being joined. The runner still has to report
          // exactly once, and must not start the command - which is exactly
          // what ProcessUtil does with a token already requested.
          runner(command, cancel, std::move(on_done));
        },
        std::move(stop));
  };
}


// Serves the manifest's outputs at `mountpoint`, blocking until torn down.
int mount(const char* mountpoint_arg) {
  const ConsoleInterruptHandler interrupt;

  const std::filesystem::path source = std::filesystem::current_path();
  const std::filesystem::path mountpoint =
      std::filesystem::absolute(mountpoint_arg);

  if (!FileSystemUtil::are_disjoint(mountpoint, source)) {
    std::println(stderr,
                 "makebelieve: mountpoint [{}] must not overlap the source "
                 "directory [{}]",
                 mountpoint_arg, source.string());
    return 1;
  }

  const RealDirectoryTree tree(source);

  // Presents the manifest's declared outputs, building each lazily through
  // the shell. The runner's working directory is the source root, so the
  // inputs it traces line up with the source's own change notifications.
  // TODO: reparse the manifest when build.makebelieve changes.
  //
  // Queues each command instead of running it on the filesystem thread, so
  // the read that triggered a build returns its placeholder at once.
  WorkQueue queue;
  const BuildDirectoryTree build_tree(
      tree, queued_runner(
                queue, BuildDirectoryTree::shell_runner(source)));

  // Declared after build_tree: the pool is joined while the tree - the target
  // of every completion callback - is still alive.
  const WorkQueue::JoinGuard joined(queue);

  const VirtualFileSystem vfs(build_tree, mountpoint_arg);

  // Constructed after the mount so the mount root exists to canonicalize.
  const UnmountChannel unmount_channel(mountpoint);

  std::println(stderr,
               "makebelieve serving [{}], press Ctrl+C or run "
               "`makebelieve unmount {}` to stop",
               mountpoint_arg, mountpoint_arg);

  block_until_any(interrupt.token(), unmount_channel.token());
  return 0;
}

// Signals the daemon serving `mountpoint` to shut down and waits for it to go.
int unmount(const char* mountpoint_arg) {
  switch (UnmountChannel::request_unmount(mountpoint_arg)) {
    case UnmountChannel::Result::ok:
      return 0;
    case UnmountChannel::Result::not_mounted:
      std::println(stderr, "makebelieve: nothing is mounted at [{}]",
                   mountpoint_arg);
      return 1;
    case UnmountChannel::Result::teardown_timeout:
      std::println(stderr,
                   "makebelieve: [{}] was told to unmount but had not torn "
                   "down after 5 seconds",
                   mountpoint_arg);
      return 1;
  }
  std::unreachable();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      return usage(argv[0]);
    }

    const std::string_view command = argv[1];
    if (command == "mount") {
      return mount(argv[2]);
    }
    if (command == "unmount") {
      return unmount(argv[2]);
    }
    return usage(argv[0]);
  } catch (const std::exception& error) {
    // Best-effort: a failure to report the failure has nowhere left to go.
    try {
      std::println(stderr, "makebelieve: {}", error.what());
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
    return 1;
  } catch (...) {
    return 1;
  }
}
