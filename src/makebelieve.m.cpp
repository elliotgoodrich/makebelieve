// SPDX-License-Identifier: MIT
#include "consoleinterrupthandler.hpp"
#include "filesystemutil.hpp"
#include "realdirectorytree.hpp"
#include "virtualfilesystem.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>
#include <semaphore>
#include <stop_token>
#include <string>
#include <utility>

int main(int argc, char** argv) {
  using namespace makebelieve;
  try {
    if (argc != 2) {
      std::println(stderr, "usage: {} <mountpoint>", argv[0]);
      return 1;
    }

    const ConsoleInterruptHandler interrupt;

    // TODO: a BuildTree belongs between these two - it will take the
    // .makebelieve manifest and this RealDirectoryTree, subscribe to its
    // changes, and present a DirectoryTree of the built outputs for the
    // filesystem to project instead of the sources directly.

    const std::filesystem::path source = std::filesystem::current_path();
    const std::filesystem::path mountpoint = std::filesystem::absolute(argv[1]);

    if (!FileSystemUtil::are_disjoint(mountpoint, source)) {
      std::println(stderr,
                   "makebelieve: mountpoint [{}] must not overlap the source "
                   "directory [{}]",
                   argv[1], source.string());
      return 1;
    }

    const RealDirectoryTree tree(source);
    const VirtualFileSystem vfs(tree, argv[1]);

    std::println(stderr, "makebelieve serving [{}], press Ctrl+C to stop",
                 argv[1]);

    // Block until the user interrupts the process.
    std::binary_semaphore stopped{0};
    const std::stop_callback callback{interrupt.token(),
                                      [&stopped] { stopped.release(); }};
    stopped.acquire();
    return 0;
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
