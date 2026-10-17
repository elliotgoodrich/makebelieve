// SPDX-License-Identifier: MIT
#include "buildcoordinator.hpp"
#include "builddirectorytree.hpp"
#include "buildpermits.hpp"
#include "buildqueue.hpp"
#include "consoleinterrupthandler.hpp"
#include "filesystemutil.hpp"
#include "iocontext.hpp"
#include "osbuildprocesstracker.hpp"
#include "realdirectorytree.hpp"
#include "shellrunner.hpp"
#include "tracer.hpp"
#include "unmountchannel.hpp"
#include "virtualfilesystem.hpp"

#include <exec/static_thread_pool.hpp>

#include <algorithm>
#include <array>
#include <charconv>
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
  std::println(stderr,
               "usage: {0} mount [-j <builds>] <mountpoint>\n"
               "       {0} mount <mountpoint> [-j <builds>]\n"
               "       {0} unmount <mountpoint>",
               program);
  return 1;
}

// A reason to stop: a token, and the exit status to end with once a stop is
// requested on it.
struct StopReason {
  std::stop_token token;
  int exit_status;
};

// Blocks until any of @a reasons has a stop requested, returning at once if one
// is already stopped, and returns the highest exit status among those stopped
// by then, so a failure stopping at the same time as a request is not lost.
// One semaphore permit per reason, so however many callbacks fire, the count
// reaches at most sizeof...(Reasons) - the semaphore's capacity.
template <std::same_as<StopReason>... Reasons>
int wait_for_stop(const Reasons&... reasons) {
  static_assert(sizeof...(Reasons) > 0, "wait_for_stop needs a reason");
  std::counting_semaphore<sizeof...(Reasons)> stopped{0};
  auto on_stop = [&stopped] { stopped.release(); };
  const std::array callbacks{std::stop_callback{reasons.token, on_stop}...};
  stopped.acquire();
  return std::max(
      {(reasons.token.stop_requested() ? reasons.exit_status : 0)...});
}

// Serves the manifest's outputs at `mountpoint`, with at most @a jobs builds
// holding a permit at once (one per core if 0), blocking until torn down.
int mount(const char* mountpoint_arg, unsigned jobs) {
  // Always recording, so a `tracing` rule added to the manifest at any point
  // shows the history leading up to it. Installed before anything below starts
  // a thread that records, and uninstalled only once they are all gone.
  Tracer tracer;
  const TracerInstallation installed_tracer(tracer);
  MB_TRACE_THREAD_NAME("main");

  // Every wait on the operating system - source changes, interrupts, unmount
  // requests - happens on this context's one thread.
  IoContext io;

  // Where the brief synchronous steps of builds, and mount notifications,
  // run. Outlives everything below, which waits out its work on the pool when
  // it goes.
  const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
  exec::static_thread_pool workers(cores);

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

  // Tracks which build each process the runner launches belongs to: the
  // runner adds its commands, and the coordinator looks up who an open comes
  // from. Outlives both, and the tree and the mount, which use them until
  // they are torn down.
  OsBuildProcessTracker tracker;

  // Runs each build through the shell. Its working directory is the source
  // root, so the inputs it traces line up with the source's own change
  // notifications.
  const ShellRunner run(source, mountpoint, io, tracker,
                        workers.get_scheduler());

  // How many builds hold a permit at once: -j, or one per core. A build whose
  // command waits on another output gives its permit back meanwhile, so this
  // limits permit-holding builds, not runnable processes or CPU use.
  BuildQueue builds(jobs != 0 ? jobs : cores, workers.get_scheduler());

  BuildCoordinator coordinator(permits_from(builds), tracker,
                               workers.get_scheduler());

  // Presents the manifest's declared outputs, building each lazily.
  const BuildDirectoryTree build_tree(
      tree, run, coordinator, [](const std::string& problems) {
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

  // A failed watch has already said why on stderr. It leaves the mount unable
  // to keep up with the source, or to be stopped as asked, so it is torn down
  // like any other stop, but ends with a failing exit status.
  return wait_for_stop(
      StopReason{.token = interrupt.token(), .exit_status = 0},
      StopReason{.token = unmount_channel.token(), .exit_status = 0},
      StopReason{.token = io.watch_failure_token(), .exit_status = 1});
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
    if (argc < 3) {
      return usage(argv[0]);
    }

    const std::string_view command = argv[1];
    if (command == "mount" && argc == 3) {
      return mount(argv[2], 0);
    }
    if (command == "mount" && argc == 5) {
      const bool jobs_first = std::string_view(argv[2]) == "-j";
      if (!jobs_first && std::string_view(argv[3]) != "-j") {
        return usage(argv[0]);
      }
      const std::string_view text = argv[jobs_first ? 3 : 4];
      unsigned jobs = 0;
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), jobs);
      if (error != std::errc() || end != text.data() + text.size() ||
          jobs == 0) {
        return usage(argv[0]);
      }
      return mount(argv[jobs_first ? 4 : 2], jobs);
    }
    if (command == "unmount" && argc == 3) {
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
