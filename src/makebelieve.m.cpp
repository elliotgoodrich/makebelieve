// SPDX-License-Identifier: MIT
#include "buildcache.hpp"
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
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <print>
#include <semaphore>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace {

using namespace makebelieve;

// Raises the soft limit on open files to the hard limit: the mount, and each
// of the many commands it may run at once, all draw on this one process's fds.
// Best-effort, as the soft limit is only a default that suits smaller programs.
void raise_open_file_limit() {
#ifndef _WIN32
  rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
      limit.rlim_cur < limit.rlim_max) {
    limit.rlim_cur = limit.rlim_max;
    ::setrlimit(RLIMIT_NOFILE, &limit);
  }
#endif
}

int usage(const char* program) {
  std::println(
      stderr,
      "usage: {0} mount [-j <builds>] [--build=requested|all] <mountpoint>\n"
      "       {0} unmount <mountpoint>\n"
      "mount's options may also follow the mountpoint",
      program);
  return 1;
}

// Where builds served at @a mountpoint are kept between runs: beside it, under
// its name with `.makebelieve` added.
std::filesystem::path cache_directory_for(std::filesystem::path mountpoint) {
  mountpoint = std::filesystem::absolute(mountpoint).lexically_normal();
  // `mnt/` names the same directory as `mnt`.
  if (!mountpoint.has_filename()) {
    mountpoint = mountpoint.parent_path();
  }
  mountpoint += ".makebelieve";
  return mountpoint;
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

// Which outputs a mount keeps built without being asked.
enum class BuildMode : std::uint8_t {
  requested,  // those that have been opened
  all,        // every output, from the moment the mount is up
};

// How `mount` was asked to serve.
struct MountOptions {
  const char* mountpoint = nullptr;
  // At most this many builds hold a permit at once; one per core if 0.
  unsigned jobs = 0;
  // Which outputs to keep built; `requested` if not given.
  std::optional<BuildMode> build;
};

// The mode @a name names, as `--build=` spells it, if any.
std::optional<BuildMode> parse_build_mode(std::string_view name) {
  if (name == "requested") {
    return BuildMode::requested;
  }
  if (name == "all") {
    return BuildMode::all;
  }
  return std::nullopt;
}

// Reads `mount`'s arguments - @a args, those after the command - or nothing if
// they are not one mountpoint and options, each given at most once.
std::optional<MountOptions> parse_mount(std::span<char* const> args) {
  constexpr std::string_view k_build_option = "--build=";
  MountOptions options;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "-j") {
      if (++i == args.size() || options.jobs != 0) {
        return std::nullopt;
      }
      const std::string_view text = args[i];
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), options.jobs);
      if (error != std::errc() || end != text.data() + text.size() ||
          options.jobs == 0) {
        return std::nullopt;
      }
    } else if (arg.starts_with(k_build_option)) {
      const std::optional<BuildMode> mode =
          parse_build_mode(arg.substr(k_build_option.size()));
      if (!mode.has_value() || options.build.has_value()) {
        return std::nullopt;
      }
      options.build = mode;
    } else if (arg.starts_with('-') || options.mountpoint != nullptr) {
      return std::nullopt;
    } else {
      options.mountpoint = args[i];
    }
  }
  if (options.mountpoint == nullptr) {
    return std::nullopt;
  }
  return options;
}

// Serves the manifest's outputs as @a options asks, blocking until torn down.
int mount(const MountOptions& options) {
  const char* const mountpoint_arg = options.mountpoint;
  const unsigned jobs = options.jobs;
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

  // Holds `makebelieve unmount` until it is destroyed, so it is declared before
  // - and outlives - everything a mount tears down. Listens once the mount is
  // up, below.
  UnmountChannel unmount_channel;

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

  // Its writes must not reach the source's change notifications, nor be taken
  // for what a build read.
  const std::filesystem::path cache_directory =
      cache_directory_for(mountpoint_arg);
  if (!FileSystemUtil::are_disjoint(cache_directory, source)) {
    std::println(stderr,
                 "makebelieve: the build cache [{}] must not overlap the "
                 "source directory [{}]",
                 cache_directory.string(), source.string());
    return 1;
  }
  // Outlives the build tree, which keeps its builds here.
  BuildCache cache(cache_directory);

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

  // Presents the manifest's declared outputs, building each lazily - except
  // those the cache still holds up to date, which it serves at once.
  const BuildDirectoryTree build_tree(
      tree, run, coordinator,
      [](const std::string& problems) {
        // Best-effort, as this runs on the IoContext's thread.
        try {
          std::println(stderr,
                       "makebelieve: ignoring the change to build.makebelieve "
                       "until it is fixed:\n{}",
                       problems);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
      },
      &cache);

  const VirtualFileSystem vfs(build_tree, mountpoint_arg,
                              workers.get_scheduler());

  // Only after the mount, so the mount root exists to canonicalize.
  unmount_channel.listen(mountpoint, io);

  // Only once the mount is up, as a command may read another output through
  // it the moment its build starts.
  if (options.build == BuildMode::all) {
    build_tree.materialize_all();
  }

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
  }
  std::unreachable();
}

}  // namespace

int main(int argc, char** argv) {
  raise_open_file_limit();
  try {
    if (argc < 3) {
      return usage(argv[0]);
    }

    const std::string_view command = argv[1];
    if (command == "mount") {
      const std::optional<MountOptions> options = parse_mount(
          std::span<char* const>(argv, static_cast<std::size_t>(argc))
              .subspan(2));
      if (!options.has_value()) {
        return usage(argv[0]);
      }
      return mount(*options);
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
