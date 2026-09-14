// SPDX-License-Identifier: MIT
#include "workqueue.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace makebelieve {

namespace {

// A token that is already stopped, for telling work that it never ran. The
// source is short-lived, but the token it hands out keeps the state alive.
std::stop_token stopped_token() {
  // Deliberately not const: libstdc++ declares stop_source::request_stop()
  // const and MSVC does not, so const satisfies clang-tidy on the one and
  // fails to compile on the other.
  // NOLINTNEXTLINE(misc-const-correctness)
  std::stop_source source;
  source.request_stop();
  return source.get_token();
}

}  // namespace

unsigned WorkQueue::default_worker_count() {
  return std::max(1U, std::thread::hardware_concurrency());
}

WorkQueue::WorkQueue(unsigned workers) {
  m_threads.reserve(workers);
  for (unsigned i = 0; i < workers; ++i) {
    m_threads.emplace_back(
        [this](const std::stop_token& stop) { run_until_stopped(stop); });
  }
}

WorkQueue::~WorkQueue() {
  // The workers refer to this queue, so they have to be gone before it is.
  stop_and_join_workers();
}

void WorkQueue::submit(Work work, std::stop_token stop) {
  {
    const std::lock_guard lock(m_mutex);
    m_jobs.push_back({.work = std::move(work), .stop = std::move(stop)});
  }
  m_ready.notify_one();
}

std::size_t WorkQueue::pending() const {
  const std::lock_guard lock(m_mutex);
  return m_jobs.size();
}

void WorkQueue::join() {
  stop_and_join_workers();

  // Only now that no worker can be holding one: run what was never started.
  // Taken under the lock and run outside it, since work runs arbitrary code
  // that must not see the queue held - it may even submit more, which the
  // loop below will not pick up but a later join or the destructor disposes of.
  std::deque<Job> pending;
  {
    const std::lock_guard lock(m_mutex);
    pending.swap(m_jobs);
  }

  // Already stopped, which is how an item is told it never ran and should
  // report its own cancellation rather than start anything.
  const std::stop_token cancelled = stopped_token();
  for (Job& job : pending) {
    job.work(cancelled);
  }
}

void WorkQueue::stop_and_join_workers() {
  // Stop every worker before joining any of them. Letting ~jthread do it would
  // stop and join one at a time, so shutting down would wait out each running
  // item in turn instead of cancelling them all at once.
  for (std::jthread& thread : m_threads) {
    thread.request_stop();
  }
  m_threads.clear();  // Joins, and leaves a second call nothing to do.
}

void WorkQueue::run_until_stopped(const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    std::unique_lock lock(m_mutex);
    if (!m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
      return;
    }
    Job job = std::move(m_jobs.front());
    m_jobs.pop_front();
    // Dropped before running: an item takes as long as it takes, and holding
    // the queue would stop every other worker picking work up - and would
    // deadlock an item that submits more from inside itself.
    lock.unlock();

    // Either the pool shutting down or this item's own token cancels it. The
    // callbacks are declared after the source so they are deregistered before
    // the source they are attached to goes.
    std::stop_source cancel;
    const std::stop_callback pool_stopped(stop,
                                          [&cancel] { cancel.request_stop(); });
    const std::stop_callback caller_stopped(
        job.stop, [&cancel] { cancel.request_stop(); });
    job.work(cancel.get_token());
  }
}

}  // namespace makebelieve
