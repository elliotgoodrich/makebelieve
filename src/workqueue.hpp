// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace makebelieve {

/// @class WorkQueue
/// A pool of threads and the queue of work feeding them.
///
/// Work that is submitted but never started is not dropped: @link join runs it
/// with an already-stopped token, which is how each item gets the chance to
/// report its own cancellation without the queue knowing how.
class WorkQueue {
 public:
  /// A unit of work, invoked exactly once with a `stop_token` requested when
  /// either the pool shuts down or the token submitted alongside it is.
  ///
  /// A token that is *already* stopped on entry means the work never got a
  /// worker and the queue is being joined: do not start anything, just report
  /// the cancellation.
  using Work = std::move_only_function<void(std::stop_token)>;

 private:
  // One submitted item, waiting for a worker to pick it up.
  struct Job {
    Work work;
    std::stop_token stop;
  };

  mutable std::mutex m_mutex;
  std::condition_variable_any m_ready;
  std::deque<Job> m_jobs;
  std::vector<std::jthread> m_threads;

 public:
  /// How many workers a queue uses unless told otherwise: one per hardware
  /// thread, and never fewer than one.
  [[nodiscard]] static unsigned default_worker_count();

  explicit WorkQueue(unsigned workers = default_worker_count());

  /// Stops and joins any worker still running. Whatever is left queued is
  /// destroyed without being run, since work that captured a reference to
  /// something already torn down must not be invoked this late. @link join is
  /// what gives it the chance to report, while that is still safe.
  ~WorkQueue();

  WorkQueue(const WorkQueue&) = delete;
  WorkQueue& operator=(const WorkQueue&) = delete;
  WorkQueue(WorkQueue&&) = delete;
  WorkQueue& operator=(WorkQueue&&) = delete;

  /// Queues @a work and returns at once. @a stop cancels this item alone,
  /// leaving the rest of the pool running. Safe to call from any thread,
  /// including from inside another item's work.
  void submit(Work work, std::stop_token stop = {});

  /// Cancels whatever is running, waits for the workers to finish, then runs
  /// everything still queued with an already-stopped token so each item can
  /// report its own cancellation.
  ///
  /// Call this while whatever the work refers to is still alive. Idempotent,
  /// and a queue that has been joined starts nothing further - though it will
  /// still accept work, which a later join or the destructor disposes of.
  void join();

  /// Returns how many items are currently waiting for a worker.
  [[nodiscard]] std::size_t pending() const;

  /// @class JoinGuard
  /// Calls @link WorkQueue::join at the end of its scope. Declare one after
  /// whatever the queued work refers to, so the pool is joined before that
  /// goes away even if the scope is left by an exception.
  class JoinGuard {
    WorkQueue& m_queue;

   public:
    explicit JoinGuard(WorkQueue& queue) : m_queue(queue) {}
    ~JoinGuard() { m_queue.join(); }

    JoinGuard(const JoinGuard&) = delete;
    JoinGuard& operator=(const JoinGuard&) = delete;
    JoinGuard(JoinGuard&&) = delete;
    JoinGuard& operator=(JoinGuard&&) = delete;
  };

 private:
  // Runs queued work until @a stop is requested.
  void run_until_stopped(const std::stop_token& stop);

  // Requests stop on every worker before joining any, then joins them all.
  void stop_and_join_workers();
};

}  // namespace makebelieve
