// SPDX-License-Identifier: MIT
#pragma once

#include "buildpermits.hpp"
#include "buildprocesstracker.hpp"
#include "commandrunner.hpp"

#include <exec/any_sender_of.hpp>
#include <exec/task.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

namespace makebelieve {

/// @class BuildCoordinator
/// Keeps build attempts that open each other's outputs from deadlocking on
/// the permits that cap how many run at once.
///
/// A build attempt is one generation of one output's build. It holds a permit
/// while its command runs, but a request from that command - one found to
/// come from the attempt by looking its process up in the
/// @link BuildProcessTracker the command was added to before it ran - that has
/// to wait for another output gives the permit back while it waits, and takes
/// one again before it is answered. So
/// N attempts each waiting on an unbuilt output leave N permits for those
/// outputs' builds, where holding on to them would deadlock.
///
/// The permits are cooperative accounting, not CPU enforcement: N limits how
/// many attempts hold a permit, not how many processes are runnable or how
/// much CPU they use. An attempt waiting on a dependency has no permit though
/// other threads, or other processes, of its command may run on - including
/// threads whose own requests were just answered, while another of its
/// requests still waits. A command that never waits on a generated output
/// keeps its permit until it completes. With permits given back, more than N
/// commands can be alive at once. Waiting requests occupy coroutine frames,
/// not dispatcher threads. There is no fixed limit on their number.
///
/// Each attempt has one permit state: held, released, or reacquiring.
/// - A request that starts to wait while its attempt holds the permit gives
///   it back. One that starts while it is released or being reacquired
///   changes nothing about the permit, but waits like any other.
/// - A request whose answer is ready is answered at once if its attempt holds
///   the permit; otherwise its attempt takes one back - or joins a take-back
///   already under way - and every request ready by then is answered together
///   once it has.
/// - Having answered, an attempt with requests still waiting on a dependency
///   gives the permit straight back, without waiting for another request:
///   its command may be waiting on those very requests (joining the thread
///   that made one, say), and holding the permit could keep what they wait on
///   from ever building. It takes one again only once another request is
///   ready, never merely because some still wait, so nothing cycles while
///   nothing new is ready.
/// - Successful dependency waits reacquire a permit before answering.
///   A canceled request returns promptly; the last such wait starts background
///   reacquisition so a continuing command restores its accounting without
///   delaying teardown.
///
/// Each wait joins one attempt of the output it needs - the generation under
/// way, or the next if the output changed since that one started - and gets
/// that attempt's result, whatever happens to the output afterwards.
///
/// A request whose wait would close a cycle of attempts waiting on each other
/// - an attempt opening its own output included - fails with
/// `std::errc::resource_deadlock_would_occur`, and every attempt on the cycle
/// is stopped, fails with that error whatever its command did, and never
/// publishes a result.
///
/// A request whose process the tracker misses - on Linux, one that has left
/// its command's process tree - waits holding no permit and giving none back,
/// and can deadlock on permits as before; there is no timeout.
///
/// Attempts that someone outside every build waits on are hurried: they go
/// ahead, in the line for a permit, of every attempt that nobody does - such
/// as a rebuild started only because an input changed. A request from no
/// attempt hurries the attempt it waits on, and everything that attempt waits
/// on, directly or through others, or waits behind; a request from a hurried
/// attempt hurries its target likewise. So a reader's build, and every
/// dependency it turns out to need, overtakes background work queued before
/// it, though never work already holding a permit. An attempt stays hurried
/// for the rest of its life. A request the tracker misses counts as being
/// from no attempt.
///
/// State changes happen under one lock, which is never held while anything
/// outside is called other than a permit's release or promotion (each of which
/// runs nothing on the calling thread) and so it may be taken while holding a
/// lock of the tree that owns it.
class BuildCoordinator {
 public:
  /// Schedules completions on an executor that outlives this coordinator.
  /// Must defer execution; an inline scheduler does not satisfy the contract.
  using Scheduler = exec::any_scheduler<exec::any_sender<exec::any_receiver<
      stdexec::completion_signatures<stdexec::set_value_t(),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>>>>;
  /// Opaque state for one generation of a build, owned by this coordinator.
  class Attempt;
  /// Shared ownership of an attempt across its build and dependent requests.
  using AttemptPtr = std::shared_ptr<Attempt>;

  /// A count of what the coordinator is tracking, for tests and traces.
  struct Stats {
    std::size_t waits = 0;        ///< Requests waiting.
    std::size_t edges = 0;        ///< Distinct attempt-to-attempt wait edges.
    std::size_t held = 0;         ///< Attempts holding a permit.
    std::size_t reacquiring = 0;  ///< Attempts taking a permit back.
  };

 private:
  // Everything the coordinator tracks, and the transitions between states.
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// Coordinates attempts that take their permits from @a permits, finding
  /// which attempt a request comes from in @a tracker - the one the runner
  /// adds each command to, under the output it builds. Completion work uses
  /// @a completions, which must defer execution off the calling thread (never
  /// an inline scheduler).
  /// @pre The permit provider, build process tracker and executor outlive this.
  BuildCoordinator(PermitProvider permits,
                   BuildProcessTracker& tracker,
                   Scheduler completions);

  /// @pre No attempt is running and no request is waiting.
  ~BuildCoordinator();

  BuildCoordinator(const BuildCoordinator&) = delete;
  BuildCoordinator& operator=(const BuildCoordinator&) = delete;
  BuildCoordinator(BuildCoordinator&&) = delete;
  BuildCoordinator& operator=(BuildCoordinator&&) = delete;

  /// A new attempt at building @a output, not started. If @a predecessor is
  /// given - the attempt of the same output under way - this one cannot start
  /// until it has ended, which a request waiting on this one waits on too.
  ///
  /// @a output names the attempt: a request the build process tracker finds
  /// to come from @a output's build is taken to come from the attempt at it
  /// that is running. So it must be the path the runner adds the attempt's
  /// command under - its `Command::output` - and at most one attempt at each
  /// output may run at a time (see `run`).
  [[nodiscard]] AttemptPtr create_attempt(std::filesystem::path output,
                                          AttemptPtr predecessor = nullptr);

  /// Runs @a work as @a attempt once it has a permit: in an environment that
  /// stops it when @a attempt is stopped (or the receiver's own stop token
  /// is).
  /// Completes with its result - with
  /// `std::errc::resource_deadlock_would_occur` instead, whatever it produced,
  /// if the attempt was on a cycle - once nothing of the attempt remains under
  /// way; an exception it throws is reported as the result of @link
  /// to_error_code.
  /// @pre @a attempt has not been run, nor completed.
  /// @pre No other attempt at @a attempt's output is running, nor will start
  /// running before this one has ended - as giving each attempt the one
  /// before it as its predecessor ensures.
  [[nodiscard]] AttemptSender run(AttemptPtr attempt, BuildSender work);

  /// Hands @a result to every request waiting on @a attempt, which then no
  /// longer blocks anyone. Called once its output is published, or once it is
  /// known it will never run.
  void complete(const AttemptPtr& attempt, std::error_code result);

  /// Suspends until the target completes and the requester's permit is back,
  /// hurrying the target if the requester is no attempt or a hurried one.
  /// Cancellation comes from the receiver environment and detaches this waiter.
  [[nodiscard]] exec::task<std::error_code> wait(
      AttemptPtr target,
      std::uint32_t requester_pid = 0);

  /// The output whose running attempt @a requester_pid belongs to, if any,
  /// as the build process tracker finds it. May read /proc, so call it holding
  /// no lock.
  [[nodiscard]] std::optional<std::filesystem::path> resolve(
      std::uint32_t requester_pid) const;

  /// As @link wait, for a request already found to come from the attempt
  /// running at @a requester (or from none), such as an in-process copy waiting
  /// as its own attempt.
  [[nodiscard]] exec::task<std::error_code> wait_as(
      AttemptPtr target,
      std::optional<std::filesystem::path> requester);

  /// What is being tracked right now.
  [[nodiscard]] Stats stats() const;

  /// How many requests from @a from are waiting on @a to, sharing one edge.
  [[nodiscard]] std::size_t edge_count(const AttemptPtr& from,
                                       const AttemptPtr& to) const;

  /// The output @a attempt builds, which names it.
  [[nodiscard]] static const std::filesystem::path& output_of(
      const AttemptPtr& attempt);
};

}  // namespace makebelieve
