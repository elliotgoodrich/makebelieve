// SPDX-License-Identifier: MIT
#include "workqueue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

using makebelieve::WorkQueue;

// Blocks until a predicate holds, so a test fails by timing out rather than
// hanging the suite when the queue never gets round to the work.
template <typename Predicate>
[[nodiscard]] bool wait_for(Predicate done) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// Blocks the work it is handed to until the test lets it go, so a test can
// hold a worker busy and queue up behind it.
class Gate {
  std::mutex m_mutex;
  std::condition_variable m_open;
  bool m_opened = false;

 public:
  void wait() {
    std::unique_lock lock(m_mutex);
    m_open.wait(lock, [this] { return m_opened; });
  }

  void open() {
    {
      const std::lock_guard lock(m_mutex);
      m_opened = true;
    }
    m_open.notify_all();
  }
};

TEST(WorkQueueTest, RunsWorkOnAnotherThread) {
  std::atomic<std::thread::id> ran_on{};
  std::atomic<bool> done = false;
  WorkQueue queue(1);

  queue.submit([&](std::stop_token) {
    ran_on = std::this_thread::get_id();
    done = true;
  });

  ASSERT_TRUE(wait_for([&] { return done.load(); }));
  EXPECT_NE(ran_on.load(), std::this_thread::get_id());
}

// The whole point of the queue: handing work over must not wait for it.
TEST(WorkQueueTest, SubmittingDoesNotWaitForTheWork) {
  Gate gate;
  std::atomic<bool> done = false;
  WorkQueue queue(1);

  queue.submit([&](std::stop_token) {
    gate.wait();
    done = true;
  });

  // The work is still blocked, yet the submit above has already returned.
  EXPECT_FALSE(done.load());
  gate.open();
  EXPECT_TRUE(wait_for([&] { return done.load(); }));
}

TEST(WorkQueueTest, IndependentWorkRunsConcurrently) {
  constexpr std::size_t k_workers = 4;
  // Every item must arrive before any may leave, so this only finishes if all
  // four run at the same time.
  std::barrier all_running(static_cast<std::ptrdiff_t>(k_workers));
  std::atomic<std::size_t> done = 0;
  WorkQueue queue(k_workers);

  for (std::size_t i = 0; i < k_workers; ++i) {
    queue.submit([&](std::stop_token) {
      all_running.arrive_and_wait();
      ++done;
    });
  }

  EXPECT_TRUE(wait_for([&] { return done.load() == k_workers; }));
}

// Work is invoked exactly once whether or not a worker ever got to it; the
// stopped token is how the queue says "you never ran".
TEST(WorkQueueTest, JoinRunsQueuedWorkWithAnAlreadyStoppedToken) {
  Gate gate;
  std::atomic<std::size_t> started = 0;
  std::atomic<std::size_t> invoked = 0;
  std::atomic<std::size_t> told_to_stop = 0;
  WorkQueue queue(1);

  for (int i = 0; i < 4; ++i) {
    queue.submit([&](std::stop_token cancel) {
      ++invoked;
      if (cancel.stop_requested()) {
        ++told_to_stop;
        return;
      }
      ++started;
      gate.wait();
    });
  }

  // Let the single worker take one item, leaving three queued behind it.
  ASSERT_TRUE(wait_for([&] { return started.load() == 1; }));
  ASSERT_TRUE(wait_for([&] { return queue.pending() == 3; }));

  gate.open();
  queue.join();

  EXPECT_EQ(invoked.load(), 4U);
  EXPECT_EQ(told_to_stop.load(), 3U);
  EXPECT_EQ(queue.pending(), 0U);
}

TEST(WorkQueueTest, JoinIsIdempotentAndStartsNothingFurther) {
  std::atomic<std::size_t> started = 0;
  WorkQueue queue(2);

  queue.join();
  queue.join();  // A second join has nothing left to do.

  std::atomic<bool> told_to_stop = false;
  queue.submit([&](std::stop_token cancel) {
    if (cancel.stop_requested()) {
      told_to_stop = true;
      return;
    }
    ++started;
  });
  // No worker is left to pick it up, so it waits for the next join.
  EXPECT_EQ(queue.pending(), 1U);
  queue.join();
  EXPECT_TRUE(told_to_stop.load());
  EXPECT_EQ(started.load(), 0U);
}

// Shutting the pool down asks work in flight to stop, so a long item does not
// hold teardown open.
TEST(WorkQueueTest, JoinRequestsStopOnRunningWork) {
  std::atomic<bool> saw_stop = false;
  std::atomic<bool> running = false;
  WorkQueue queue(1);

  queue.submit([&](std::stop_token cancel) {
    running = true;
    while (!cancel.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    saw_stop = true;
  });

  ASSERT_TRUE(wait_for([&] { return running.load(); }));
  queue.join();
  EXPECT_TRUE(saw_stop.load());
}

// The token submitted alongside an item cancels just that one, leaving the
// pool - and everything else on it - running.
TEST(WorkQueueTest, AnItemsOwnStopTokenCancelsOnlyThatItem) {
  std::atomic<bool> first_saw_stop = false;
  std::atomic<bool> second_ran = false;
  WorkQueue queue(2);

  std::stop_source cancel_first;
  queue.submit(
      [&](std::stop_token cancel) {
        while (!cancel.stop_requested()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        first_saw_stop = true;
      },
      cancel_first.get_token());
  queue.submit([&](std::stop_token) { second_ran = true; });

  ASSERT_TRUE(wait_for([&] { return second_ran.load(); }));
  EXPECT_FALSE(first_saw_stop.load());

  cancel_first.request_stop();
  EXPECT_TRUE(wait_for([&] { return first_saw_stop.load(); }));
}

// Work may queue more work from inside itself without deadlocking on the queue.
TEST(WorkQueueTest, WorkCanSubmitMoreWork) {
  std::atomic<bool> nested_ran = false;
  WorkQueue queue(2);

  queue.submit([&](std::stop_token) {
    queue.submit([&](std::stop_token) { nested_ran = true; });
  });

  EXPECT_TRUE(wait_for([&] { return nested_ran.load(); }));
}

// Destroying a queue that was never joined must still stop and join its
// threads - they refer to it - but must not run what is left, since that work
// may capture something already gone by then.
TEST(WorkQueueTest, DestructionWithoutJoinDiscardsQueuedWorkUnrun) {
  std::atomic<bool> cancelled_in_flight = false;
  std::atomic<bool> running = false;
  std::atomic<bool> queued_ran = false;
  {
    WorkQueue queue(1);
    queue.submit([&](std::stop_token cancel) {
      running = true;
      while (!cancel.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cancelled_in_flight = true;
    });
    ASSERT_TRUE(wait_for([&] { return running.load(); }));
    // Queued behind the one in flight, so it is still waiting at destruction.
    queue.submit([&](std::stop_token) { queued_ran = true; });
  }
  EXPECT_TRUE(cancelled_in_flight.load());
  EXPECT_FALSE(queued_ran.load());
}

TEST(WorkQueueTest, JoinGuardJoinsAtTheEndOfItsScope) {
  std::atomic<bool> told_to_stop = false;
  std::optional<WorkQueue> queue;
  queue.emplace(1);
  {
    const WorkQueue::JoinGuard joined(*queue);
    queue->join();  // Leaves no worker, so the item below is never started.
    queue->submit([&](std::stop_token cancel) {
      told_to_stop = cancel.stop_requested();
    });
    EXPECT_FALSE(told_to_stop.load());
  }
  EXPECT_TRUE(told_to_stop.load());
}

}  // namespace
