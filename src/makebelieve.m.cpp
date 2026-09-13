// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"
#include "consoleinterrupthandler.hpp"
#include "filesystemutil.hpp"
#include "realdirectorytree.hpp"
#include "unmountchannel.hpp"
#include "virtualfilesystem.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <print>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
  struct BuildJob {
    std::string command;
    std::stop_token stop;
    BuildDirectoryTree::BuildComplete complete;
  };
  std::mutex jobs_mutex;
  std::condition_variable_any jobs_ready;
  std::deque<BuildJob> jobs;
  const BuildDirectoryTree build_tree(
      tree, [&](std::string command, std::stop_token stop,
                BuildDirectoryTree::BuildComplete complete) {
        {
          const std::lock_guard lock(jobs_mutex);
          jobs.push_back(
              {std::move(command), std::move(stop), std::move(complete)});
        }
        jobs_ready.notify_one();
      });

  // ProcessUtil blocks. Run commands outside the filesystem callback so the
  // initial read can return its placeholder before the mount updates that
  // file. One worker per core, since the jobs are independent - each command
  // gets its own scratch directory, and BuildDirectoryTree already keeps a
  // second build of the same output off the queue.
  //
  // Declared after build_tree: cancellation and joining happen while the tree
  // (and every completion callback's target) is still alive.
  const auto worker = [&](const std::stop_token& stop) {
    const BuildDirectoryTree::CommandRunner run =
        BuildDirectoryTree::shell_runner(source);
    while (!stop.stop_requested()) {
      std::unique_lock lock(jobs_mutex);
      if (!jobs_ready.wait(lock, stop, [&] { return !jobs.empty(); })) {
        return;
      }
      BuildJob job = std::move(jobs.front());
      jobs.pop_front();
      lock.unlock();
      std::stop_source command_stop;
      const std::stop_callback worker_stopped(
          stop, [&] { command_stop.request_stop(); });
      const std::stop_callback tree_stopped(
          job.stop, [&] { command_stop.request_stop(); });
      run(job.command, command_stop.get_token(), std::move(job.complete));
    }
  };
  std::vector<std::jthread> builders;
  for (unsigned i = std::max(1U, std::thread::hardware_concurrency()); i != 0;
       --i) {
    builders.emplace_back(worker);
  }

  // Destroyed before `builders`, so every worker is told to stop before the
  // vector joins them one at a time - otherwise shutdown waits out each
  // running command in turn rather than cancelling them all at once.
  const struct StopBuilders {
    std::vector<std::jthread>& builders;
    ~StopBuilders() {
      for (std::jthread& builder : builders) {
        builder.request_stop();
      }
    }
  } stop_builders{builders};

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
