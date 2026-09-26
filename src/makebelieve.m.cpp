// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"
#include "consoleinterrupthandler.hpp"
#include "filesystemutil.hpp"
#include "iocontext.hpp"
#include "realdirectorytree.hpp"
#include "tracer.hpp"
#include "unmountchannel.hpp"
#include "virtualfilesystem.hpp"

#include <exec/static_thread_pool.hpp>

#include <algorithm>
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
#include <thread>
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

// Serves the manifest's outputs at `mountpoint`, blocking until torn down.
int mount(const char* mountpoint_arg) {
  // Always recording, so a `tracing` rule added to the manifest at any point
  // shows the history leading up to it. Installed before anything below starts
  // a thread that records, and uninstalled only once they are all gone.
  Tracer tracer;
  const TracerInstallation installed_tracer(tracer);
  MB_TRACE_THREAD_NAME("main");

  // Every wait on the operating system - source changes, interrupts, unmount
  // requests - happens on this context's one thread.
  IoContext io;

  // Where builds and mount notifications run: one thread per core, a build
  // blocking its thread for as long as the command it runs. Outlives
  // everything below, which waits out its work on the pool when it goes.
  exec::static_thread_pool workers(
      std::max(1U, std::thread::hardware_concurrency()));

  const ConsoleInterruptHandler interrupt(io);

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

  const RealDirectoryTree tree(source, io);

  // Presents the manifest's declared outputs, building each lazily through
  // the shell. The runner's working directory is the source root, so the
  // inputs it traces line up with the source's own change notifications.
  const BuildDirectoryTree build_tree(
      tree, BuildDirectoryTree::shell_runner(source, workers.get_scheduler()),
      [](const std::string& problems) {
        // Best-effort, as this runs on the IoContext's thread.
        try {
          std::println(stderr,
                       "makebelieve: ignoring the change to build.makebelieve "
                       "until it is fixed:\n{}",
                       problems);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
      });

  const VirtualFileSystem vfs(build_tree, mountpoint_arg,
                              workers.get_scheduler());

  // Constructed after the mount so the mount root exists to canonicalize.
  const UnmountChannel unmount_channel(mountpoint, io);

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
