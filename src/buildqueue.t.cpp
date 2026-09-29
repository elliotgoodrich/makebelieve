// SPDX-License-Identifier: MIT
#include "buildqueue.hpp"

#include <gtest/gtest.h>
#include <exec/single_thread_context.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {

using namespace makebelieve;
using namespace std::chrono_literals;

namespace ex = stdexec;

// Work that, once started, holds its slot until the test lets it finish, and
// records the order work started in.
class Held {
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::vector<int> m_started;
  std::set<int> m_released;
  int m_stopped = 0;

  // Runs each one's blocking part on a thread of its own, so waiting here
  // holds none of the threads the queue's own completions need, nor any other
  // work's. Not a shared pool: static_thread_pool queues work scheduled from
  // outside it on a random thread, which alone runs it, so it could wait behind
  // work that is being held.
  std::unordered_map<int, std::unique_ptr<exec::single_thread_context>>
      m_runners;

 public:
  // Work that records @a who as started, then waits to be released.
  [[nodiscard]] auto work(int who) {
    const auto runner = [&] {
      const std::lock_guard lock(m_mutex);
      auto& context = m_runners[who];
      if (context == nullptr) {
        context = std::make_unique<exec::single_thread_context>();
      }
      return context->get_scheduler();
    }();
    return ex::schedule(runner) | ex::then([this, who]() noexcept {
             std::unique_lock lock(m_mutex);
             m_started.push_back(who);
             m_changed.notify_all();
             m_changed.wait(lock, [&] { return m_released.contains(who); });
           });
  }

  void release(int who) {
    {
      const std::lock_guard lock(m_mutex);
      m_released.insert(who);
    }
    m_changed.notify_all();
  }

  void stopped() {
    {
      const std::lock_guard lock(m_mutex);
      ++m_stopped;
    }
    m_changed.notify_all();
  }

  // Waits (bounded) until @a started have started and @a stopped stopped.
  [[nodiscard]] bool wait_until(std::size_t started, int stopped = 0) {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, 10s, [&] {
      return m_started.size() >= started && m_stopped >= stopped;
    });
  }

  [[nodiscard]] std::vector<int> started() {
    const std::lock_guard lock(m_mutex);
    return m_started;
  }
};

template <class Queue>
void spawn_held(Queue& queue, ex::counting_scope& scope, Held& held, int who) {
  ex::spawn(queue.schedule(held.work(who)) |
                ex::upon_stopped([&held]() noexcept { held.stopped(); }) |
                ex::upon_error([](const auto&) noexcept {}),
            scope.get_token());
}

// Leased work driven a step at a time by the test, each step run with its
// lease on a thread of the work's own, until told to finish.
template <class Queue>
class Driven {
 public:
  using Lease = typename Queue::Lease;
  using Step = std::function<void(Lease&)>;

 private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::deque<Step> m_steps;
  int m_steps_done = 0;
  bool m_started = false;
  bool m_finish = false;
  exec::single_thread_context m_runner;

  void run(Lease& lease) {
    std::unique_lock lock(m_mutex);
    m_started = true;
    m_changed.notify_all();
    while (true) {
      m_changed.wait(lock, [&] { return m_finish || !m_steps.empty(); });
      if (m_steps.empty()) {
        return;
      }
      Step step = std::move(m_steps.front());
      m_steps.pop_front();
      lock.unlock();
      step(lease);
      lock.lock();
      ++m_steps_done;
      m_changed.notify_all();
    }
  }

 public:
  // Starts the work through @a queue, in @a scope.
  void spawn(Queue& queue, ex::counting_scope& scope) {
    ex::spawn(queue.schedule_leased([this](Lease& lease) {
      return ex::schedule(m_runner.get_scheduler()) |
             ex::then([this, &lease]() noexcept { run(lease); });
    }) | ex::upon_stopped([]() noexcept {}) |
                  ex::upon_error([](const auto&) noexcept {}),
              scope.get_token());
  }

  // Queues @a step for the work to run.
  void post(Step step) {
    {
      const std::lock_guard lock(m_mutex);
      m_steps.push_back(std::move(step));
    }
    m_changed.notify_all();
  }

  // Waits (bounded) until the work has started and run @a steps steps.
  [[nodiscard]] bool wait_until(int steps) {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(
        lock, 10s, [&] { return m_started && m_steps_done >= steps; });
  }

  // Lets the work complete once its steps are done.
  void finish() {
    {
      const std::lock_guard lock(m_mutex);
      m_finish = true;
    }
    m_changed.notify_all();
  }
};

// Checks @a queue runs exactly @a slots pieces of work at once: that many
// start, and one more does not until one of them finishes.
template <class Queue>
void expect_slots(Queue& queue, int slots) {
  Held held;
  ex::counting_scope scope;
  for (int i = 0; i <= slots; ++i) {
    spawn_held(queue, scope, held, i);
  }
  EXPECT_TRUE(held.wait_until(slots));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(held.started().size(), static_cast<std::size_t>(slots));
  for (int i = 0; i <= slots; ++i) {
    held.release(i);
  }
  ex::sync_wait(scope.join());
}

std::thread::id thread_of(exec::static_thread_pool& pool) {
  return std::get<0>(
      ex::sync_wait(ex::schedule(pool.get_scheduler()) |
                    ex::then([] { return std::this_thread::get_id(); }))
          .value());
}

TEST(BuildQueue, CompletesAsTheWorkDoes) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());

  const auto value = ex::sync_wait(queue.schedule(ex::just(42)));
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(std::get<0>(*value), 42);

  const auto error = ex::sync_wait(
      queue.schedule(
          ex::just_error(std::make_error_code(std::errc::io_error))) |
      ex::upon_error([](const auto& error) noexcept {
        if constexpr (std::same_as<std::decay_t<decltype(error)>,
                                   std::error_code>) {
          return error;
        } else {
          return std::error_code{};  // Not expected: the work never throws.
        }
      }));
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(std::get<0>(*error), std::errc::io_error);

  const auto stopped =
      ex::sync_wait(queue.schedule(ex::just_stopped()) |
                    ex::upon_stopped([]() noexcept { return true; }));
  ASSERT_TRUE(stopped.has_value());
  EXPECT_TRUE(std::get<0>(*stopped));
}

// A slot comes back when the work completes, not when its operation state is
// destroyed: within when_all the first piece's state lives until the second
// has completed, which needs that slot.
TEST(BuildQueue, ASlotComesBackWhenTheWorkCompletes) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());
  const auto both = ex::sync_wait(
      ex::when_all(queue.schedule(ex::just(1)), queue.schedule(ex::just(2))));
  ASSERT_TRUE(both.has_value());
  EXPECT_EQ(*both, std::tuple(1, 2));
}

// Records how a piece of work completed, leaving its operation state alive.
struct Recorder {
  using receiver_concept = ex::receiver_t;
  bool* completed;
  void set_value() noexcept { *completed = true; }
  void set_stopped() noexcept { *completed = true; }
  void set_error(std::exception_ptr) noexcept { *completed = true; }
};

TEST(BuildQueue, ACompletedOperationKeptAliveHoldsNoSlot) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());

  bool first = false;
  auto kept = ex::connect(queue.schedule(ex::just()), Recorder{&first});
  ex::start(kept);
  ASSERT_TRUE(first);

  // The slot is free again, so this starts, and completes, inline.
  bool second = false;
  auto next = ex::connect(queue.schedule(ex::just()), Recorder{&second});
  ex::start(next);
  EXPECT_TRUE(second);
}

TEST(BuildQueue, RunsNoMoreThanItsLimitAtOnce) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(2, pool.get_scheduler());
  Held held;
  ex::counting_scope scope;
  for (int who = 1; who <= 3; ++who) {
    spawn_held(queue, scope, held, who);
  }

  ASSERT_TRUE(held.wait_until(2));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(held.started().size(), 2U);  // The third waits for a slot.

  held.release(1);
  ASSERT_TRUE(held.wait_until(3));
  held.release(2);
  held.release(3);
  ex::sync_wait(scope.join());
}

TEST(BuildQueue, StartsWaitingWorkInTheOrderItWasScheduled) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());
  Held held;
  ex::counting_scope scope;
  for (int who = 1; who <= 4; ++who) {
    spawn_held(queue, scope, held, who);
  }

  for (int who = 1; who <= 4; ++who) {
    ASSERT_TRUE(held.wait_until(static_cast<std::size_t>(who)));
    held.release(who);
  }
  ex::sync_wait(scope.join());
  EXPECT_EQ(held.started(), (std::vector<int>{1, 2, 3, 4}));
}

// Work stopped while it waits leaves the queue without starting, completes
// stopped on the queue's scheduler, and its turn goes to the next in line.
TEST(BuildQueue, WorkStoppedWhileWaitingLeavesTheQueue) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());
  Held held;
  ex::counting_scope scope;
  spawn_held(queue, scope, held, 1);
  ASSERT_TRUE(held.wait_until(1));

  ex::inplace_stop_source stop;
  std::optional<std::thread::id> stopped_on;
  std::thread waiter([&] {
    const auto result = ex::sync_wait(
        ex::write_env(queue.schedule(held.work(2)),
                      ex::prop{ex::get_stop_token, stop.get_token()}) |
        ex::upon_stopped(
            [&]() noexcept { stopped_on = std::this_thread::get_id(); }));
    static_cast<void>(result);
  });
  std::this_thread::sleep_for(50ms);
  spawn_held(queue, scope, held, 3);

  stop.request_stop();
  waiter.join();
  EXPECT_EQ(stopped_on, thread_of(pool));

  held.release(1);
  ASSERT_TRUE(held.wait_until(2));
  EXPECT_EQ(held.started(), (std::vector<int>{1, 3}));
  held.release(3);
  ex::sync_wait(scope.join());
}

TEST(BuildQueue, WorkStoppedBeforeItIsScheduledNeverStarts) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());
  bool ran = false;
  ex::inplace_stop_source stop;
  stop.request_stop();
  const auto result = ex::sync_wait(ex::write_env(
      queue.schedule(ex::just() | ex::then([&ran]() noexcept { ran = true; })),
      ex::prop{ex::get_stop_token, stop.get_token()}));
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(ran);

  // The slot it never took is still free.
  EXPECT_TRUE(ex::sync_wait(queue.schedule(ex::just())).has_value());
}

// Stops racing slots changing hands for the same waiter lose no slot: the
// work runs on other threads, so it is still giving slots back, and waiters
// are still being handed them, while the stop takes the rest off the queue.
TEST(BuildQueue, StopsRacingReleasesLoseNoSlot) {
  exec::static_thread_pool pool(1);
  exec::static_thread_pool runners(4);
  BuildQueue queue(2, pool.get_scheduler());
  constexpr int k_work = 200;
  std::atomic<int> finished = 0;
  {
    ex::counting_scope scope;
    for (int i = 0; i < k_work; ++i) {
      ex::spawn(
          queue.schedule(
              ex::schedule(runners.get_scheduler()) | ex::then([]() noexcept {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
              })) |
              ex::then([&finished]() noexcept { ++finished; }) |
              ex::upon_stopped([&finished]() noexcept { ++finished; }) |
              ex::upon_error([](const auto&) noexcept {}),
          scope.get_token());
    }
    std::this_thread::sleep_for(2ms);
    scope.request_stop();
    ex::sync_wait(scope.join());
  }
  EXPECT_EQ(finished.load(), k_work);

  // Both slots came back: two pieces of held work run side by side.
  Held held;
  ex::counting_scope scope;
  spawn_held(queue, scope, held, 1);
  spawn_held(queue, scope, held, 2);
  EXPECT_TRUE(held.wait_until(2));
  held.release(1);
  held.release(2);
  ex::sync_wait(scope.join());
}

// Waiters whose work completes at once each resume on the handoff scheduler,
// not one within another, so however many there are the stack does not grow.
TEST(BuildQueue, HandsSlotsToManyInlineWaitersWithoutRecursing) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());
  Held held;
  ex::counting_scope scope;
  spawn_held(queue, scope, held, 1);
  ASSERT_TRUE(held.wait_until(1));

  constexpr int k_waiters = 20000;
  std::atomic<int> completed = 0;
  for (int i = 0; i != k_waiters; ++i) {
    ex::spawn(queue.schedule(ex::just()) | ex::then([&completed]() noexcept {
                ++completed;
              }) | ex::upon_error([](const auto&) noexcept {}),
              scope.get_token());
  }
  held.release(1);
  ex::sync_wait(scope.join());
  EXPECT_EQ(completed.load(), k_waiters);
}

// A slot given back is free at once, whatever the thread that gave it back
// does next - here, a continuation that waits on more work through the same
// queue. Bounded, so a regression fails rather than hangs.
TEST(BuildQueue, ASlotGivenBackIsFreeWhateverFollowsTheWork) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());
  Held held;
  ex::counting_scope scope;
  spawn_held(queue, scope, held, 1);
  ASSERT_TRUE(held.wait_until(1));

  std::atomic<bool> inner_completed = false;
  ex::spawn(queue.schedule(ex::just()) | ex::then([&]() noexcept {
              ex::inplace_stop_source timeout;
              const std::jthread stopper([&timeout] {
                std::this_thread::sleep_for(2s);
                timeout.request_stop();
              });
              inner_completed =
                  ex::sync_wait(ex::write_env(queue.schedule(ex::just()),
                                              ex::prop{ex::get_stop_token,
                                                       timeout.get_token()}))
                      .has_value();
            }) | ex::upon_error([](const auto&) noexcept {}),
            scope.get_token());
  held.release(1);
  ex::sync_wait(scope.join());
  EXPECT_TRUE(inner_completed);
}

// Work that had to wait starts on the handoff scheduler, not on the thread
// that gave its slot back.
TEST(BuildQueue, StartsWorkThatWaitedOnItsHandoffScheduler) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(1, pool.get_scheduler());
  Held held;
  ex::counting_scope scope;
  spawn_held(queue, scope, held, 1);
  ASSERT_TRUE(held.wait_until(1));

  std::optional<std::thread::id> started_on;
  ex::spawn(queue.schedule(ex::just() | ex::then([&started_on]() noexcept {
                             started_on = std::this_thread::get_id();
                           })) |
                ex::upon_error([](const auto&) noexcept {}),
            scope.get_token());
  held.release(1);
  ex::sync_wait(scope.join());
  EXPECT_EQ(started_on, thread_of(pool));
}

using PoolQueue = BuildQueue<exec::static_thread_pool::scheduler>;

// Giving a lease's slot back starts work waiting for one at once, and taking
// it back waits for that work to finish.
TEST(BuildQueue, ReleasingALeaseStartsAQueuedWaiterAtOnce) {
  exec::static_thread_pool pool(1);
  PoolQueue queue(1, pool.get_scheduler());
  Driven<PoolQueue> leased;
  Held held;
  ex::counting_scope scope;
  leased.spawn(queue, scope);
  ASSERT_TRUE(leased.wait_until(0));
  spawn_held(queue, scope, held, 1);
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(held.started().empty());

  leased.post([](PoolQueue::Lease& lease) { lease.release(); });
  ASSERT_TRUE(held.wait_until(1));

  std::atomic<bool> reacquired = false;
  std::atomic<bool> held_after = false;
  leased.post([&](PoolQueue::Lease& lease) {
    reacquired = ex::sync_wait(lease.reacquire()).has_value();
    held_after = lease.held();
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(reacquired);
  held.release(1);
  ASSERT_TRUE(leased.wait_until(2));
  EXPECT_TRUE(reacquired);
  EXPECT_TRUE(held_after);
  leased.finish();
  ex::sync_wait(scope.join());
  expect_slots(queue, 1);
}

// A lease taking its slot back goes ahead of new work that was queued first.
TEST(BuildQueue, AReacquireIsServedBeforeNewWork) {
  exec::static_thread_pool pool(1);
  PoolQueue queue(1, pool.get_scheduler());
  Driven<PoolQueue> leased;
  Held held;
  ex::counting_scope scope;
  leased.spawn(queue, scope);
  ASSERT_TRUE(leased.wait_until(0));
  leased.post([](PoolQueue::Lease& lease) { lease.release(); });
  spawn_held(queue, scope, held, 1);
  ASSERT_TRUE(held.wait_until(1));

  // New work queues first, then the lease asks for its slot back.
  spawn_held(queue, scope, held, 2);
  std::atomic<bool> reacquired = false;
  leased.post([&](PoolQueue::Lease& lease) {
    reacquired = ex::sync_wait(lease.reacquire()).has_value();
  });
  std::this_thread::sleep_for(50ms);

  held.release(1);
  ASSERT_TRUE(leased.wait_until(2));
  EXPECT_TRUE(reacquired);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(held.started(), (std::vector<int>{1}));

  leased.finish();
  ASSERT_TRUE(held.wait_until(2));
  held.release(2);
  ex::sync_wait(scope.join());
  expect_slots(queue, 1);
}

// A reacquire stopped while it waits completes stopped on the handoff
// scheduler, holding nothing, and no slot goes missing.
TEST(BuildQueue, AStoppedReacquireCompletesStoppedOnHandoffLeakingNothing) {
  exec::static_thread_pool pool(1);
  PoolQueue queue(1, pool.get_scheduler());
  Driven<PoolQueue> leased;
  Held held;
  ex::counting_scope scope;
  leased.spawn(queue, scope);
  ASSERT_TRUE(leased.wait_until(0));
  leased.post([](PoolQueue::Lease& lease) { lease.release(); });
  spawn_held(queue, scope, held, 1);
  ASSERT_TRUE(held.wait_until(1));

  ex::inplace_stop_source stop;
  std::optional<std::thread::id> stopped_on;
  std::atomic<bool> held_after = true;
  leased.post([&](PoolQueue::Lease& lease) {
    const auto result = ex::sync_wait(
        ex::write_env(lease.reacquire(),
                      ex::prop{ex::get_stop_token, stop.get_token()}) |
        ex::upon_stopped(
            [&]() noexcept { stopped_on = std::this_thread::get_id(); }));
    static_cast<void>(result);
    held_after = lease.held();
  });
  std::this_thread::sleep_for(50ms);
  stop.request_stop();
  ASSERT_TRUE(leased.wait_until(2));
  EXPECT_EQ(stopped_on, thread_of(pool));
  EXPECT_FALSE(held_after);

  leased.finish();
  held.release(1);
  ex::sync_wait(scope.join());
  expect_slots(queue, 1);
}

// Leases giving slots back and taking them again, racing stops that take
// their reacquires off the queue, lose no slot and make none up.
TEST(BuildQueue, StopsRacingReleasesAndReacquiresLoseNoSlot) {
  exec::static_thread_pool pool(1);
  exec::static_thread_pool runners(4);
  constexpr int k_slots = 3;
  PoolQueue queue(k_slots, pool.get_scheduler());
  constexpr int k_work = 200;
  std::atomic<int> finished = 0;
  std::atomic<int> holding = 0;
  std::atomic<int> most_holding = 0;
  {
    ex::counting_scope scope;
    for (int i = 0; i < k_work; ++i) {
      ex::spawn(queue.schedule_leased([&](PoolQueue::Lease& lease) {
        // Hold the slot for a moment, give it back, take it again.
        auto round = [&]() {
          return ex::schedule(runners.get_scheduler()) |
                 ex::then([&]() noexcept {
                   const int now = ++holding;
                   int most = most_holding.load();
                   while (now > most &&
                          !most_holding.compare_exchange_weak(most, now)) {
                   }
                   std::this_thread::sleep_for(std::chrono::microseconds(50));
                   --holding;
                   lease.release();
                 }) |
                 ex::let_value([&lease] { return lease.reacquire(); });
        };
        return ex::just() | ex::let_value(round) | ex::let_value(round) |
               ex::let_value(round) | ex::let_value(round);
      }) | ex::then([&finished]() noexcept { ++finished; }) |
                    ex::upon_stopped([&finished]() noexcept { ++finished; }) |
                    ex::upon_error([](const auto&) noexcept {}),
                scope.get_token());
    }
    std::this_thread::sleep_for(5ms);
    scope.request_stop();
    ex::sync_wait(scope.join());
  }
  EXPECT_EQ(finished.load(), k_work);
  EXPECT_LE(most_holding.load(), k_slots);
  expect_slots(queue, k_slots);
}

}  // namespace
