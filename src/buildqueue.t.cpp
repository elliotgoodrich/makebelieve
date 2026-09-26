// SPDX-License-Identifier: MIT
#include "buildqueue.hpp"

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
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

  // Runs the blocking part on a pool of its own, so waiting here holds none of
  // the threads the queue's own completions need.
  exec::static_thread_pool m_runners{8};

 public:
  // Work that records @a who as started, then waits to be released.
  [[nodiscard]] auto work(int who) {
    return ex::schedule(m_runners.get_scheduler()) |
           ex::then([this, who]() noexcept {
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

// Stops racing slots changing hands for the same waiter lose no slot.
TEST(BuildQueue, StopsRacingReleasesLoseNoSlot) {
  exec::static_thread_pool pool(1);
  BuildQueue queue(2, pool.get_scheduler());
  constexpr int k_work = 200;
  std::atomic<int> finished = 0;
  {
    ex::counting_scope scope;
    for (int i = 0; i < k_work; ++i) {
      ex::spawn(queue.schedule(ex::just()) |
                    ex::then([&finished]() noexcept { ++finished; }) |
                    ex::upon_stopped([&finished]() noexcept { ++finished; }) |
                    ex::upon_error([](const auto&) noexcept {}),
                scope.get_token());
    }
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

}  // namespace
