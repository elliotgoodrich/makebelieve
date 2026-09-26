// SPDX-License-Identifier: MIT
#pragma once

#include "builddirectorytree.hpp"
#include "iocontext.hpp"

#include <exec/static_thread_pool.hpp>

#include <filesystem>

namespace makebelieve {

/// @class ShellRunner
/// A `BuildDirectoryTree::CommandRunner` that carries out each rule it is
/// given, with no limit of its own on how many run at once. Its
/// brief synchronous steps - preparing a scratch directory, starting a
/// command, reading back what it wrote - run on a worker pool, and it waits on
/// a command through an `IoContext`, holding no thread while it runs.
///
/// Each rule runs through the system shell with the working directory set to
/// the one given at construction, so relative inputs resolve against it. A
/// `Run` command has `%out` substituted with a scratch file - with the same
/// file name as the output it builds, so a tool that chooses its format from
/// the extension needs no extra flag - and produces the bytes it wrote there;
/// a `Capture` command produces the bytes it wrote to standard output. A
/// `Copy` rule runs no command at all: it produces the bytes of the file it
/// names, read from under the working directory, and reports that file as its
/// one input, so a copy is tracked even where command tracing is unavailable.
/// Traced inputs are reported relative to the working directory, so for
/// dependency tracking to line up it should be the filesystem root that the
/// tree's source mirrors.
class ShellRunner {
  std::filesystem::path m_working_directory;
  IoContext* m_io;
  exec::static_thread_pool::scheduler m_workers;

 public:
  /// Creates a runner working in @a working_directory, running its
  /// synchronous steps on @a workers and waiting through @a io.
  /// @pre @a io and the pool behind @a workers outlive every build it starts.
  ShellRunner(std::filesystem::path working_directory,
              IoContext& io,
              exec::static_thread_pool::scheduler workers);

  /// The build of @a command, as this class describes.
  [[nodiscard]] BuildDirectoryTree::BuildSender operator()(
      BuildDirectoryTree::Command command) const;
};

}  // namespace makebelieve
