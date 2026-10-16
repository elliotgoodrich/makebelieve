// SPDX-License-Identifier: MIT
#pragma once

#include <stdexec/execution.hpp>

#include <concepts>
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
// 1. BuildCoordinator starts a build's sender with a BuildProcessOwner - the
//    BuildProcessTracker and the output being built - in the sender's
//    environment, under `get_build_process_owner`.
// 2. ShellRunner reads it from there and passes it to ProcessUtil::run.
// 3. ProcessUtil::run creates the command's process paused, describes it
//    with a ProcessHandle, and adds it to the tracker through the
//    BuildProcessOwner.
//    It lets the process run only once added, and keeps the
//    ProcessRemoveGuard it got back until the command and everything it
//    started are gone.
// 4. Meanwhile, a request from any of those processes reaches the build
//    directory tree with the requester's id, which BuildCoordinator looks up
//    in the same BuildProcessTracker.
// 5. Destroying the ProcessRemoveGuard removes the command from the tracker:
//    the build's processes are no longer found.
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
/// Code that adds a process does not call `remove` itself: it holds a
/// @link ProcessRemoveGuard, which calls it on destruction, so the record is
/// removed on every way out.
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

/// @class ProcessRemoveGuard
/// Removes a process from a @link BuildProcessTracker when destroyed.
///
/// `BuildProcessOwner::add` returns one for each process it adds. Whoever
/// launched the process keeps it for exactly as long as the process, or
/// anything it started, might make a request that should be found -
/// `ProcessUtil::run` holds it until the command has exited and whatever it
/// started has been ended (on Windows, waited for too) - and then destroys
/// it, after which the build's processes are no longer found. That ordering
/// is what keeps a recycled process id from being found to belong to a build
/// that has ended. Holding a guard rather than calling `remove` by hand means
/// no early return can leave a record behind.
///
/// A default-constructed guard removes nothing. Move-only: each record has
/// one guard. The tracker must outlive it.
class ProcessRemoveGuard {
  BuildProcessTracker* m_tracker = nullptr;
  BuildProcessTracker::Token m_token = 0;

 public:
  /// Removes nothing.
  ProcessRemoveGuard() = default;

  /// Removes @a token from @a tracker on destruction.
  ProcessRemoveGuard(BuildProcessTracker& tracker,
                     BuildProcessTracker::Token token) noexcept
      : m_tracker(&tracker), m_token(token) {}

  /// Takes @a other's record, leaving it removing nothing.
  ProcessRemoveGuard(ProcessRemoveGuard&& other) noexcept
      : m_tracker(std::exchange(other.m_tracker, nullptr)),
        m_token(other.m_token) {}

  /// Takes @a other's record and removes this object's previous one.
  ProcessRemoveGuard& operator=(ProcessRemoveGuard other) noexcept {
    std::swap(m_tracker, other.m_tracker);
    std::swap(m_token, other.m_token);
    return *this;
  }

  ProcessRemoveGuard(const ProcessRemoveGuard&) = delete;

  /// Removes the record, if any.
  ~ProcessRemoveGuard() {
    if (m_tracker != nullptr) {
      m_tracker->remove(m_token);
    }
  }
};

/// @class BuildProcessOwner
/// The build that owns the processes a launch starts: the
/// @link BuildProcessTracker to add them to, and the output to add them under.
///
/// The BuildCoordinator makes one for each build it starts and hands it to
/// the build through `get_build_process_owner`. `ShellRunner` passes it to
/// `ProcessUtil::run`, which calls `add` with the command's process after
/// creating it paused and before letting it run, so that nothing the command
/// does can come before it is in the tracker. A launcher has no need to
/// interpret the output; it only passes it on.
///
/// There is no default constructor: a launch that no build owns says so with
/// `BuildProcessOwner::none()`, so that it is always a deliberate choice.
///
/// Copying it copies the output's path. The tracker must outlive every copy.
class BuildProcessOwner {
  BuildProcessTracker* m_tracker;
  std::filesystem::path m_output;

 public:
  /// Adds to @a tracker as the build of @a output.
  BuildProcessOwner(BuildProcessTracker& tracker, std::filesystem::path output)
      : m_tracker(&tracker), m_output(std::move(output)) {}

  /// Owned by no build: adds nothing.
  [[nodiscard]] static BuildProcessOwner none() {
    return {BuildProcessTracker::none(), {}};
  }

  /// The output of the build that owns the processes.
  [[nodiscard]] const std::filesystem::path& output() const noexcept {
    return m_output;
  }

  /// Records that @a process, and what it starts, belong to this build, until
  /// the guard returned is destroyed - or says why it could not, in which
  /// case the process must never be let run.
  /// @pre @a process has not yet run any of its own code.
  [[nodiscard]] std::expected<ProcessRemoveGuard, std::error_code> add(
      const ProcessHandle& process) const {
    return m_tracker->add(m_output, process)
        .transform([this](BuildProcessTracker::Token token) {
          return ProcessRemoveGuard(*m_tracker, token);
        });
  }
};

/// @class get_build_process_owner_t
/// The query for the @link BuildProcessOwner a build sender must add every
/// process it launches under.
///
/// The tracker belongs to the BuildCoordinator, which only meets a build's
/// sender when it starts it, after the @link CommandRunner made it, so it
/// supplies the owner through the sender's environment rather than as an
/// argument. The query has no default: a receiver whose environment does not
/// answer it cannot run a build sender, so no wrapper along the way can drop
/// it and have processes silently go missing from the tracker. It is forwarded
/// through sender adaptors.
struct get_build_process_owner_t {
  /// Obtains the required owner from @a env; there is no fallback.
  template <class Env>
    requires requires(const Env& env, const get_build_process_owner_t& query) {
      { env.query(query) } noexcept -> std::convertible_to<BuildProcessOwner>;
    }
  [[nodiscard]] BuildProcessOwner operator()(const Env& env) const noexcept {
    return env.query(*this);
  }

  /// Marks this query for forwarding through sender environment adaptors.
  [[nodiscard]] static consteval bool query(stdexec::forwarding_query_t) {
    return true;
  }
};

/// Queries the owner of the processes a process-launching sender starts.
inline constexpr get_build_process_owner_t get_build_process_owner{};

}  // namespace makebelieve
