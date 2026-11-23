// SPDX-License-Identifier: MIT
#include "buildcache.hpp"
#include "buildcoordinator.hpp"
#include "builddirectorytree.hpp"
#include "buildpermits.hpp"
#include "buildqueue.hpp"
#include "consoleinterrupthandler.hpp"
#include "dependencygraph.hpp"
#include "filesystemutil.hpp"
#include "iocontext.hpp"
#include "osbuildprocesstracker.hpp"
#include "realdirectorytree.hpp"
#include "scopeguard.hpp"
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
#include <expected>
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
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
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
      "usage: {0} mount [-j <builds>] [--build=requested|all] <directory>\n"
      "       {0} unmount <directory>\n"
      "       {0} deps [-i|--invert] [--depth <n>] "
      "[--prefix <indent|depth|none>]\n"
      "            [--charset <utf8|ascii>] [--no-dedupe] [--all]\n"
      "            <directory> <path>...\n"
      "outputs are served at <directory>/mount and kept in\n"
      "<directory>/.makebelieve; mount's options may also follow the "
      "directory",
      program);
  return 1;
}

// Where the outputs of the build directory @a directory are served.
std::filesystem::path mountpoint_for(const std::filesystem::path& directory) {
  return directory / "mount";
}

// Where the builds served from @a directory are kept between runs.
std::filesystem::path cache_directory_for(
    const std::filesystem::path& directory) {
  return directory / ".makebelieve";
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
  // Holds the mount and the cache.
  const char* directory = nullptr;
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
// they are not one directory and options, each given at most once.
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
    } else if (arg.starts_with('-') || options.directory != nullptr) {
      return std::nullopt;
    } else {
      options.directory = args[i];
    }
  }
  if (options.directory == nullptr) {
    return std::nullopt;
  }
  return options;
}

// Serves the manifest's outputs as @a options asks, blocking until torn down.
int mount(const MountOptions& options) {
  const char* const directory_arg = options.directory;
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
  const std::filesystem::path directory =
      std::filesystem::absolute(directory_arg).lexically_normal();

  // It holds the mount and the cache, whose writes must not reach the source's
  // change notifications, nor be taken for what a build read.
  if (!FileSystemUtil::are_disjoint(directory, source)) {
    std::println(stderr,
                 "makebelieve: directory [{}] must not be, contain or be "
                 "inside the source directory [{}], the one makebelieve was "
                 "started in; serve from somewhere else, such as "
                 "`makebelieve mount ../output`",
                 directory.string(), source.string());
    return 1;
  }
  const std::filesystem::path mountpoint = mountpoint_for(directory);
  const std::filesystem::path cache_directory = cache_directory_for(directory);

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
  // notifications. It builds into the cache's scratch directory, from where
  // what it built is moved into the cache and served.
  const ShellRunner run(source, mountpoint, cache.scratch_directory(), io,
                        tracker, workers.get_scheduler());

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
      &cache,
      [](const std::string& report) {
        // Best-effort, as this runs wherever the build finished - or, for a
        // path more than one rule claims, on the IoContext's thread. One
        // print, so builds finishing at once do not interleave.
        try {
          std::print(stderr, "makebelieve: {}", report);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
      });

  const VirtualFileSystem vfs(build_tree, mountpoint, workers.get_scheduler());

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
               mountpoint_for(directory_arg).string(), directory_arg);

  // A failed watch has already said why on stderr. It leaves the mount unable
  // to keep up with the source, or to be stopped as asked, so it is torn down
  // like any other stop, but ends with a failing exit status.
  return wait_for_stop(
      StopReason{.token = interrupt.token(), .exit_status = 0},
      StopReason{.token = unmount_channel.token(), .exit_status = 0},
      StopReason{.token = io.watch_failure_token(), .exit_status = 1});
}

// Signals the daemon serving from @a directory_arg to shut down and waits for
// it to go.
int unmount(const char* directory_arg) {
  switch (UnmountChannel::request_unmount(mountpoint_for(directory_arg))) {
    case UnmountChannel::Result::ok:
      return 0;
    case UnmountChannel::Result::not_mounted:
      std::println(stderr, "makebelieve: nothing is mounted from [{}]",
                   directory_arg);
      return 1;
  }
  std::unreachable();
}

// How `deps` was asked to print.
struct DepsOptions {
  // Holds the cache.
  const char* directory = nullptr;
  // The paths to print the dependencies of.
  std::vector<std::string> paths;
  DependencyGraph::Options graph;
};

// The prefix style @a name names, as `--prefix` spells it, if any.
std::optional<DependencyGraph::Prefix> parse_prefix(std::string_view name) {
  if (name == "indent") {
    return DependencyGraph::Prefix::indent;
  }
  if (name == "depth") {
    return DependencyGraph::Prefix::depth;
  }
  if (name == "none") {
    return DependencyGraph::Prefix::none;
  }
  return std::nullopt;
}

// The charset @a name names, as `--charset` spells it, if any.
std::optional<DependencyGraph::Charset> parse_charset(std::string_view name) {
  if (name == "utf8") {
    return DependencyGraph::Charset::utf8;
  }
  if (name == "ascii") {
    return DependencyGraph::Charset::ascii;
  }
  return std::nullopt;
}

// Reads `deps`' arguments - @a args, those after the command - or nothing if
// they are not a directory, at least one path and options, each given at most
// once. An option's value may be the next argument or follow an `=`.
std::optional<DepsOptions> parse_deps(std::span<char* const> args) {
  DepsOptions options;
  bool prefix_given = false;
  bool charset_given = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    const std::string_view name = arg.substr(0, arg.find('='));
    if (name == "--depth" || name == "--prefix" || name == "--charset") {
      std::string_view value;
      if (name.size() != arg.size()) {
        value = arg.substr(name.size() + 1);
      } else if (++i < args.size()) {
        value = args[i];
      } else {
        return std::nullopt;
      }
      if (name == "--depth") {
        std::size_t depth = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), depth);
        if (error != std::errc() || end != value.data() + value.size() ||
            options.graph.depth.has_value()) {
          return std::nullopt;
        }
        options.graph.depth = depth;
      } else if (name == "--charset") {
        const std::optional<DependencyGraph::Charset> charset =
            parse_charset(value);
        if (!charset.has_value() || charset_given) {
          return std::nullopt;
        }
        options.graph.charset = *charset;
        charset_given = true;
      } else {
        const std::optional<DependencyGraph::Prefix> prefix =
            parse_prefix(value);
        if (!prefix.has_value() || prefix_given) {
          return std::nullopt;
        }
        options.graph.prefix = *prefix;
        prefix_given = true;
      }
    } else if (arg == "-i" || arg == "--invert") {
      if (options.graph.invert) {
        return std::nullopt;
      }
      options.graph.invert = true;
    } else if (arg == "--no-dedupe") {
      if (options.graph.no_dedupe) {
        return std::nullopt;
      }
      options.graph.no_dedupe = true;
    } else if (arg == "--all") {
      if (options.graph.all) {
        return std::nullopt;
      }
      options.graph.all = true;
    } else if (arg.starts_with('-')) {
      return std::nullopt;
    } else if (options.directory == nullptr) {
      options.directory = args[i];
    } else {
      options.paths.emplace_back(arg);
    }
  }
  if (options.directory == nullptr || options.paths.empty()) {
    return std::nullopt;
  }
  return options;
}

// Writes the UTF-8 @a text to standard output. A Windows console decodes what
// it is given by its output code page, which is rarely UTF-8, so that is
// switched for the write and put back after.
void print_utf8(std::string_view text) {
#ifdef _WIN32
  // Zero if there is no console.
  const UINT code_page = ::GetConsoleOutputCP();
  if (code_page != 0) {
    ::SetConsoleOutputCP(CP_UTF8);
  }
  const ScopeGuard restore([code_page] {
    if (code_page != 0) {
      ::SetConsoleOutputCP(code_page);
    }
  });
#endif
  std::print("{}", text);
  std::fflush(stdout);
}

// Prints what @a options asks about the builds kept in its directory, which
// may be being served from at the time.
int deps(const DepsOptions& options) {
  const std::vector<BuildCache::Record> records =
      BuildCache::read_records(cache_directory_for(options.directory));
  const DependencyGraph graph(records);
  const std::expected<std::string, std::string> printed =
      graph.print(options.paths, options.graph);
  if (!printed.has_value()) {
    std::println(stderr, "makebelieve: {}", printed.error());
    return 1;
  }
  print_utf8(*printed);
  return 0;
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
    if (command == "deps") {
      const std::optional<DepsOptions> options = parse_deps(
          std::span<char* const>(argv, static_cast<std::size_t>(argc))
              .subspan(2));
      if (!options.has_value()) {
        return usage(argv[0]);
      }
      return deps(*options);
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
