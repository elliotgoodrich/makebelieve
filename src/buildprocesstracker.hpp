// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

// Telling which build, if any, a filesystem request came from.
//
// When a build's command opens a generated file - `cc` building `app.o` opens
// `@/gen.h`, say - the operating system tells us only the id of the process
// that asked. But it matters which build that process belongs to: the build
// directory tree records the file as that build's dependency, spots a cycle
// if the file's own build is waiting on it in turn, and has the build give its
// permit back while it waits (see BuildCoordinator). A request from outside
// any build - an editor, a shell - needs none of that.
//
// So every command a build launches is added to a tracker, under the output the
// build makes, before it runs any code; a request's process id is looked up in
// that tracker; and the command is removed again once it, and everything it
// started, has gone. Only the command itself is added: the processes it goes
// on to start are found through how the operating system groups processes (a
// job on Windows, the parent chain on Linux).
//
// A build is named by its output. That is unambiguous because builds of the
// same output never run at once: a rebuild starts only once the build before
// it has ended.
//
// In order:
//
// 1. One BuildProcessTracker is made for the mount, and given both to the
//    ShellRunner, which adds commands to it, and to the BuildCoordinator,
//    which looks requesters up in it.
// 2. ShellRunner passes it, and the output the command builds
//    (`Command::output`), to ProcessUtil::run.
// 3. ProcessUtil::run creates the command's process paused, describes it
//    with a ProcessHandle, and adds it to the tracker under that output. It
//    lets the process run only once added, and has a ScopeGuard remove it
//    again once the command and everything it started are gone.
// 4. Meanwhile, a request from any of those processes reaches the build
//    directory tree with the requester's id, which BuildCoordinator looks up
//    in the tracker. The output it finds names the running attempt, because
//    the tree names each attempt by its rule's output, the same path it hands
//    the runner as `Command::output`.
// 5. Once removed, the build's processes are no longer found.
//
// This header is the whole contract between those parts, and depends on none
// of them.

namespace makebelieve {

/// @class ProcessHandle
/// A live process: its id, and an operating-system handle that pins down
/// which process that id means. An id alone becomes ambiguous once its
/// process exits and the id is reused; the handle does not. Each platform has
/// its own kind of handle, so besides `pid` the struct has only the field the
/// platform it is built for uses:
///
/// - Windows: `job`, the job object the process is in. Nothing can leave it,
///   so it holds everything the process starts, too.
/// - Linux: `pidfd`, a handle to the process itself.
///
/// The process must still be alive wherever a `ProcessHandle` is used. The
/// handles are borrowed: valid only for the duration of the call they are
/// passed to. Code that needs one for longer duplicates it.
///
/// Made by `ProcessUtil::run`; read by `OsBuildProcessTracker`.
struct ProcessHandle {
  /// The process id.
  std::uint32_t pid = 0;

#ifdef _WIN32
  /// The job object the process, and everything it starts, belongs to (a
  /// `HANDLE`).
  void* job = nullptr;
#else
  /// A pidfd for the process.
  int pidfd = -1;
#endif
};

/// @class BuildProcessTracker
/// Tracks which build each running process belongs to, with a build named
/// by the output it makes. An interface:
///
/// - `add` records that a process, and everything it goes on to start,
///   belongs to the build of an output, and returns a `Token` naming
///   that record. It is called once per command, before the command runs.
/// - `remove` takes that `Token` and deletes the record, after which the
///   process and what it started are no longer found.
/// - `find` takes the id of a process that made a request and returns the
///   output whose build it belongs to, if any.
///
/// All three may be called from any thread, at the same time.
///
/// Whoever adds a process keeps it in the tracker for exactly as long as the
/// process, or anything it started, might make a request that should be
/// found, and removes it then: `ProcessUtil::run` removes it once the command
/// has exited and whatever it started has been killed and has exited too.
/// Removing it no earlier is what keeps a recycled process id from
/// being found to belong to a build that has ended. It calls `remove` from a
/// `ScopeGuard`, so that no early return can leave a record behind.
///
/// Implemented by `OsBuildProcessTracker`, the real tracker, which knows how
/// each platform groups processes; and by fakes in the tests.
/// `BuildProcessTracker::none()` is an always-empty tracker, for code that runs
/// builds without needing to tell their processes apart.
class BuildProcessTracker {
 public:
  /// Names one record to the tracker that made it, for `remove`. Meaningful
  /// only to that tracker, which chooses it.
  using Token = std::uint64_t;

  /// An always-empty tracker: adding records nothing, and no process is found
  /// to belong to any build. Lives for the whole program.
  [[nodiscard]] static BuildProcessTracker& none() noexcept;

  virtual ~BuildProcessTracker() = default;

  /// Records that @a process, and what it starts, belong to the build of
  /// @a output, until `remove` is called with the token returned - or says
  /// why it could not, in which case the process must never be let run.
  /// @pre @a process has not yet run any of its own code, so nothing it does
  /// can come before it is in the tracker.
  [[nodiscard]] virtual std::expected<Token, std::error_code> add(
      const std::filesystem::path& output,
      const ProcessHandle& process) = 0;

  /// Deletes the record `add` returned @a token for.
  /// @pre @a token came from this tracker's `add`, and has not been removed.
  virtual void remove(Token token) noexcept = 0;

  /// The output whose build @a requester_pid belongs to - because it is a
  /// process that was added, or descends from one - if any.
  [[nodiscard]] virtual std::optional<std::filesystem::path> find(
      std::uint32_t requester_pid) = 0;

 protected:
  BuildProcessTracker() = default;
  BuildProcessTracker(const BuildProcessTracker&) = default;
  BuildProcessTracker& operator=(const BuildProcessTracker&) = default;
  BuildProcessTracker(BuildProcessTracker&&) = default;
  BuildProcessTracker& operator=(BuildProcessTracker&&) = default;
};

inline BuildProcessTracker& BuildProcessTracker::none() noexcept {
  class Empty final : public BuildProcessTracker {
   public:
    std::expected<Token, std::error_code> add(
        const std::filesystem::path& /*output*/,
        const ProcessHandle& /*process*/) override {
      return Token{0};
    }

    void remove(Token /*token*/) noexcept override {}

    std::optional<std::filesystem::path> find(
        std::uint32_t /*requester_pid*/) override {
      return std::nullopt;
    }
  };
  static Empty empty;
  return empty;
}

}  // namespace makebelieve
