// SPDX-License-Identifier: MIT
#include "iocontext.hpp"

#include <stdexec/execution.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace {

using namespace makebelieve;
using namespace std::chrono_literals;

namespace ex = stdexec;

// A handle the test can make ready and unready: an eventfd on Linux, a
// manual-reset event on Windows.
class Signal {
#ifdef _WIN32
  HANDLE m_handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#else
  int m_handle = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif

 public:
  Signal() = default;
  ~Signal() {
#ifdef _WIN32
    CloseHandle(m_handle);
#else
    ::close(m_handle);
#endif
  }

  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;
  Signal(Signal&&) = delete;
  Signal& operator=(Signal&&) = delete;

  [[nodiscard]] IoContext::NativeHandle handle() const { return m_handle; }

  void set() const {
#ifdef _WIN32
    SetEvent(m_handle);
#else
    const std::uint64_t one = 1;
    const ssize_t written = ::write(m_handle, &one, sizeof(one));
    static_cast<void>(written);
#endif
  }

  void reset() const {
#ifdef _WIN32
    ResetEvent(m_handle);
#else
    std::uint64_t value = 0;
    const ssize_t drained = ::read(m_handle, &value, sizeof(value));
    static_cast<void>(drained);
#endif
  }
};

// What a wait ended with.
enum class Outcome { ready, error, stopped };

// Waits on @a signal, stoppable through @a stop, recording where it
// completed.
Outcome wait_on(IoContext& io,
                const Signal& signal,
                ex::inplace_stop_token stop = {},
                std::thread::id* completed_on = nullptr) {
  const auto record = [completed_on]() noexcept {
    if (completed_on != nullptr) {
      *completed_on = std::this_thread::get_id();
    }
  };
  auto result =
      ex::sync_wait(ex::write_env(io.async_wait(signal.handle()),
                                  ex::prop{ex::get_stop_token, stop}) |
                    ex::then([record]() noexcept {
                      record();
                      return Outcome::ready;
                    }) |
                    ex::upon_error([record](std::error_code) noexcept {
                      record();
                      return Outcome::error;
                    }) |
                    ex::upon_stopped([record]() noexcept {
                      record();
                      return Outcome::stopped;
                    }));
  return std::get<0>(result.value());
}

// Watches @a handle, calling @a on_ready, stoppable through @a stop, recording
// where it completed - `Outcome::ready` standing for no value.
template <class OnReady>
Outcome watch_on(IoContext& io,
                 IoContext::NativeHandle handle,
                 OnReady on_ready,
                 ex::inplace_stop_token stop = {},
                 std::thread::id* completed_on = nullptr) {
  const auto record = [completed_on]() noexcept {
    if (completed_on != nullptr) {
      *completed_on = std::this_thread::get_id();
    }
  };
  auto result =
      ex::sync_wait(ex::write_env(io.watch(handle, std::move(on_ready)),
                                  ex::prop{ex::get_stop_token, stop}) |
                    ex::then([record]() noexcept {
                      record();
                      return Outcome::ready;
                    }) |
                    ex::upon_stopped([record]() noexcept {
                      record();
                      return Outcome::stopped;
                    }));
  return std::get<0>(result.value());
}

std::thread::id context_thread(IoContext& io) {
  return std::get<0>(
      ex::sync_wait(ex::schedule(io.get_scheduler()) |
                    ex::then([] { return std::this_thread::get_id(); }))
          .value());
}

// Waits up to 10s for @a count to reach @a expected, returning whether it did.
bool wait_for_count(const std::atomic<int>& count, int expected) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (count < expected) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

TEST(IoContext, SchedulerRunsWorkOnTheContextsThread) {
  IoContext io;
  const std::thread::id first = context_thread(io);
  EXPECT_NE(first, std::this_thread::get_id());
  EXPECT_EQ(context_thread(io), first);
}

TEST(IoContext, WaitCompletesOnceTheHandleIsReady) {
  IoContext io;
  const Signal signal;
  std::thread setter([&signal] {
    std::this_thread::sleep_for(50ms);
    signal.set();
  });
  std::thread::id completed_on;
  EXPECT_EQ(wait_on(io, signal, {}, &completed_on), Outcome::ready);
  setter.join();
  EXPECT_EQ(completed_on, context_thread(io));
}

TEST(IoContext, WaitCompletesAtOnceWhenTheHandleIsAlreadyReady) {
  IoContext io;
  const Signal signal;
  signal.set();
  EXPECT_EQ(wait_on(io, signal), Outcome::ready);
}

TEST(IoContext, WaitsCanBeRepeatedOnOneHandle) {
  IoContext io;
  const Signal signal;
  for (int i = 0; i < 100; ++i) {
    signal.set();
    ASSERT_EQ(wait_on(io, signal), Outcome::ready);
    signal.reset();
  }
}

TEST(IoContext, WaitsOnDifferentHandlesCompleteIndependently) {
  IoContext io;
  const Signal first;
  const Signal second;
  std::optional<Outcome> first_outcome;
  std::thread waiter([&] { first_outcome = wait_on(io, first); });

  second.set();
  EXPECT_EQ(wait_on(io, second), Outcome::ready);
  EXPECT_FALSE(first_outcome.has_value());

  first.set();
  waiter.join();
  EXPECT_EQ(first_outcome, Outcome::ready);
}

TEST(IoContext, StopCancelsAWaitFromTheContextsThread) {
  IoContext io;
  const Signal signal;
  ex::inplace_stop_source stop;
  std::thread::id completed_on;
  std::optional<Outcome> outcome;
  std::thread waiter(
      [&] { outcome = wait_on(io, signal, stop.get_token(), &completed_on); });

  std::this_thread::sleep_for(50ms);
  stop.request_stop();
  waiter.join();
  EXPECT_EQ(outcome, Outcome::stopped);
  EXPECT_EQ(completed_on, context_thread(io));
}

TEST(IoContext, AWaitStoppedBeforeItStartsCompletesStopped) {
  IoContext io;
  const Signal signal;
  ex::inplace_stop_source stop;
  stop.request_stop();
  std::thread::id completed_on;
  EXPECT_EQ(wait_on(io, signal, stop.get_token(), &completed_on),
            Outcome::stopped);
  EXPECT_EQ(completed_on, context_thread(io));
}

TEST(IoContext, ScopeStopEndsASpawnedWaitLoop) {
  IoContext io;
  const Signal signal;
  int wakes = 0;
  ex::counting_scope scope;
  ex::spawn(io.async_wait(signal.handle()) | ex::then([&]() noexcept {
              ++wakes;
              signal.reset();
            }) | ex::upon_error([](std::error_code) noexcept {}),
            scope.get_token());
  signal.set();
  std::this_thread::sleep_for(50ms);
  scope.request_stop();
  ex::sync_wait(scope.join());
  EXPECT_EQ(wakes, 1);
}

TEST(IoContext, SpawnWatchCallsBackEachTimeUntilStopped) {
  IoContext io;
  const Signal signal;
  std::mutex mutex;
  std::condition_variable woken;
  int wakes = 0;
  ex::counting_scope scope;
  io.spawn_watch(signal.handle(), scope, [&]() noexcept {
    signal.reset();
    {
      const std::lock_guard lock(mutex);
      ++wakes;
    }
    woken.notify_all();
  });

  for (int expected = 1; expected <= 3; ++expected) {
    signal.set();
    std::unique_lock lock(mutex);
    ASSERT_TRUE(woken.wait_for(lock, 10s, [&] { return wakes >= expected; }));
  }

  scope.request_stop();
  ex::sync_wait(scope.join());
  signal.set();
  std::this_thread::sleep_for(50ms);
  const std::lock_guard lock(mutex);
  EXPECT_EQ(wakes, 3);
}

TEST(IoContext, WatchCallsBackEachTimeUntilStopped) {
  IoContext io;
  const Signal signal;
  std::mutex mutex;
  std::condition_variable woken;
  int wakes = 0;
  ex::counting_scope scope;
  ex::spawn(io.watch(signal.handle(),
                     [&]() noexcept {
                       signal.reset();
                       {
                         const std::lock_guard lock(mutex);
                         ++wakes;
                       }
                       woken.notify_all();
                     }),
            scope.get_token());

  for (int expected = 1; expected <= 3; ++expected) {
    signal.set();
    std::unique_lock lock(mutex);
    ASSERT_TRUE(woken.wait_for(lock, 10s, [&] { return wakes >= expected; }));
  }

  scope.request_stop();
  ex::sync_wait(scope.join());
  signal.set();
  std::this_thread::sleep_for(50ms);
  const std::lock_guard lock(mutex);
  EXPECT_EQ(wakes, 3);
}

TEST(IoContext, WatchCallsBackOncePerEventOnTheContextsThread) {
  IoContext io;
  const Signal signal;
  const std::thread::id io_thread = context_thread(io);
  std::atomic<int> wakes = 0;
  std::atomic<int> elsewhere = 0;
  ex::counting_scope scope;
  io.spawn_watch(signal.handle(), scope, [&]() noexcept {
    signal.reset();
    if (std::this_thread::get_id() != io_thread) {
      ++elsewhere;
    }
    ++wakes;
  });

  constexpr int k_events = 1000;
  for (int expected = 1; expected <= k_events; ++expected) {
    signal.set();
    ASSERT_TRUE(wait_for_count(wakes, expected));
  }
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(wakes.load(), k_events);
  EXPECT_EQ(elsewhere.load(), 0);

  scope.request_stop();
  ex::sync_wait(scope.join());
}

TEST(IoContext, StopEndsARegisteredWatchOnTheContextsThread) {
  IoContext io;
  const Signal signal;
  ex::inplace_stop_source stop;
  std::atomic<int> wakes = 0;
  std::thread::id completed_on;
  std::optional<Outcome> outcome;
  std::thread watcher([&] {
    outcome = watch_on(
        io, signal.handle(),
        [&]() noexcept {
          signal.reset();
          ++wakes;
        },
        stop.get_token(), &completed_on);
  });

  // Once called back, the watch is certainly registered.
  signal.set();
  ASSERT_TRUE(wait_for_count(wakes, 1));
  stop.request_stop();
  watcher.join();
  EXPECT_EQ(outcome, Outcome::stopped);
  EXPECT_EQ(completed_on, context_thread(io));
  EXPECT_EQ(wakes.load(), 1);
}

TEST(IoContext, AWatchStoppedBeforeItStartsCompletesStopped) {
  IoContext io;
  const Signal signal;
  signal.set();
  ex::inplace_stop_source stop;
  stop.request_stop();
  int wakes = 0;
  std::thread::id completed_on;
  EXPECT_EQ(watch_on(
                io, signal.handle(), [&]() noexcept { ++wakes; },
                stop.get_token(), &completed_on),
            Outcome::stopped);
  EXPECT_EQ(completed_on, context_thread(io));
  EXPECT_EQ(wakes, 0);
}

// The handle is left ready, so the watch would be called back again were it
// not stopped first.
TEST(IoContext, AWatchCanBeStoppedFromItsCallback) {
  IoContext io;
  const Signal signal;
  signal.set();
  ex::inplace_stop_source stop;
  int wakes = 0;
  std::thread::id completed_on;
  EXPECT_EQ(watch_on(
                io, signal.handle(),
                [&]() noexcept {
                  ++wakes;
                  stop.request_stop();
                },
                stop.get_token(), &completed_on),
            Outcome::stopped);
  EXPECT_EQ(completed_on, context_thread(io));
  EXPECT_EQ(wakes, 1);
}

TEST(IoContext, ASpawnedWatchCanStopItsScopeFromItsCallback) {
  IoContext io;
  const Signal signal;
  std::atomic<int> wakes = 0;
  ex::counting_scope scope;
  io.spawn_watch(signal.handle(), scope, [&]() noexcept {
    ++wakes;
    scope.request_stop();
  });
  signal.set();
  ex::sync_wait(scope.join());
  EXPECT_EQ(wakes.load(), 1);
}

TEST(IoContext, AnUnwaitableHandleCompletesAWatch) {
  IoContext io;
#ifdef _WIN32
  const IoContext::NativeHandle bad = nullptr;
#else
  const IoContext::NativeHandle bad = -1;
#endif
  int wakes = 0;
  std::thread::id completed_on;
  EXPECT_EQ(watch_on(
                io, bad, [&]() noexcept { ++wakes; }, {}, &completed_on),
            Outcome::ready);
  EXPECT_EQ(completed_on, context_thread(io));
  EXPECT_EQ(wakes, 0);
}

// Far more than the 63 handles one WaitForMultipleObjects can watch.
TEST(IoContext, WaitsOnManyHandlesAtOnce) {
  IoContext io;
  constexpr int k_handles = 200;
  std::vector<std::unique_ptr<Signal>> signals;
  for (int i = 0; i < k_handles; ++i) {
    signals.push_back(std::make_unique<Signal>());
  }

  std::atomic<int> ready = 0;
  ex::counting_scope scope;
  for (const std::unique_ptr<Signal>& signal : signals) {
    ex::spawn(io.async_wait(signal->handle()) | ex::then([&ready]() noexcept {
                ++ready;
              }) | ex::upon_error([](std::error_code) noexcept {}),
              scope.get_token());
  }
  for (const std::unique_ptr<Signal>& signal : signals) {
    signal->set();
  }

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (ready < k_handles && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(ready.load(), k_handles);
  scope.request_stop();
  ex::sync_wait(scope.join());
}

TEST(IoContext, StoppingSomeWaitsLeavesTheOthersWaiting) {
  IoContext io;
  constexpr int k_waits = 20;
  std::vector<std::unique_ptr<Signal>> signals;
  std::vector<std::unique_ptr<ex::inplace_stop_source>> stops;
  for (int i = 0; i < k_waits; ++i) {
    signals.push_back(std::make_unique<Signal>());
    stops.push_back(std::make_unique<ex::inplace_stop_source>());
  }

  std::atomic<int> ready = 0;
  std::atomic<int> stopped = 0;
  ex::counting_scope scope;
  for (int i = 0; i < k_waits; ++i) {
    ex::spawn(
        ex::write_env(io.async_wait(signals[i]->handle()),
                      ex::prop{ex::get_stop_token, stops[i]->get_token()}) |
            ex::then([&ready]() noexcept { ++ready; }) |
            ex::upon_error([](std::error_code) noexcept {}) |
            ex::upon_stopped([&stopped]() noexcept { ++stopped; }),
        scope.get_token());
  }
  std::this_thread::sleep_for(50ms);

  const auto wait_for = [](const std::atomic<int>& count, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (count < expected && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(10ms);
    }
  };

  for (int i = 0; i < k_waits; i += 2) {
    stops[i]->request_stop();
  }
  wait_for(stopped, k_waits / 2);
  EXPECT_EQ(stopped.load(), k_waits / 2);
  EXPECT_EQ(ready.load(), 0);

  for (int i = 1; i < k_waits; i += 2) {
    signals[i]->set();
  }
  wait_for(ready, k_waits / 2);
  EXPECT_EQ(ready.load(), k_waits / 2);
  EXPECT_EQ(stopped.load(), k_waits / 2);
  ex::sync_wait(scope.join());
}

TEST(IoContext, AnUnwaitableHandleFailsTheWait) {
  IoContext io;
#ifdef _WIN32
  const IoContext::NativeHandle bad = nullptr;
#else
  const IoContext::NativeHandle bad = -1;
#endif
  const auto result = ex::sync_wait(
      io.async_wait(bad) | ex::then([]() noexcept { return false; }) |
      ex::upon_error([](std::error_code error) noexcept {
        return static_cast<bool>(error);
      }));
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::get<0>(*result));
}

// Waits a random, short while - not at all, a yield, or a spin of up to 20us
// - to vary how the caller's next step interleaves with the context's thread.
void jitter(std::mt19937& rng) {
  switch (rng() % 3) {
    case 0:
      return;
    case 1:
      std::this_thread::yield();
      return;
    default: {
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::microseconds(rng() % 20);
      while (std::chrono::steady_clock::now() < until) {
      }
    }
  }
}

// Races handles becoming ready, stops being requested and waits starting
// against each other from many threads at once, alongside waits that stay
// registered throughout, for the sanitizers to catch a race or a wait touched
// after it completes. Each wait must complete exactly once, in a way its
// readiness and stop allow.
TEST(IoContext, StressRacesReadinessAgainstStops) {
  IoContext io;

  constexpr int k_background = 50;
  std::vector<std::unique_ptr<Signal>> background_signals;
  ex::inplace_stop_source background_stop;
  std::atomic<int> background_stopped = 0;
  ex::counting_scope background;
  for (int i = 0; i < k_background; ++i) {
    background_signals.push_back(std::make_unique<Signal>());
    ex::spawn(ex::write_env(
                  io.async_wait(background_signals.back()->handle()),
                  ex::prop{ex::get_stop_token, background_stop.get_token()}) |
                  ex::then([]() noexcept {}) |
                  ex::upon_error([](std::error_code) noexcept {}) |
                  ex::upon_stopped([&background_stopped]() noexcept {
                    ++background_stopped;
                  }),
              background.get_token());
  }

  constexpr int k_threads = 8;
  constexpr int k_iterations = 500;
  std::atomic<int> wrong = 0;
  std::vector<std::thread> workers;
  for (int t = 0; t < k_threads; ++t) {
    workers.emplace_back([&io, &wrong, t] {
      std::mt19937 rng(t);
      for (int i = 0; i < k_iterations; ++i) {
        // At least one of the two, so that every wait ends.
        const bool set = rng() % 4 != 0;
        const bool stop = !set || rng() % 2 == 0;
        const bool before_start = rng() % 4 == 0;

        const Signal signal;
        ex::inplace_stop_source source;
        const auto act = [&] {
          if (set && stop && rng() % 2 == 0) {
            source.request_stop();
            jitter(rng);
            signal.set();
            return;
          }
          if (set) {
            signal.set();
          }
          if (set && stop) {
            jitter(rng);
          }
          if (stop) {
            source.request_stop();
          }
        };

        std::atomic<int> completions = 0;
        std::atomic<Outcome> outcome = Outcome::error;
        const auto record = [&](Outcome result) noexcept {
          outcome = result;
          ++completions;
        };

        if (before_start) {
          act();
        }
        ex::counting_scope scope;
        ex::spawn(
            ex::write_env(io.async_wait(signal.handle()),
                          ex::prop{ex::get_stop_token, source.get_token()}) |
                ex::then([&]() noexcept { record(Outcome::ready); }) |
                ex::upon_error(
                    [&](std::error_code) noexcept { record(Outcome::error); }) |
                ex::upon_stopped([&]() noexcept { record(Outcome::stopped); }),
            scope.get_token());
        if (!before_start) {
          jitter(rng);
          act();
        }
        ex::sync_wait(scope.join());

        const bool allowed = (outcome == Outcome::ready && set) ||
                             (outcome == Outcome::stopped && stop);
        if (completions != 1 || !allowed) {
          ++wrong;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(wrong.load(), 0);

  background_stop.request_stop();
  ex::sync_wait(background.join());
  EXPECT_EQ(background_stopped.load(), k_background);
}

// As above, for watches: handles becoming ready, again and again, race stops
// requested from outside, from the watch's own callback, and before it
// starts, alongside watches that are called back throughout. Each watch must
// complete stopped exactly once, after no more callbacks than events, and none
// after a stop its own callback requested.
TEST(IoContext, StressRacesWatchesAgainstStops) {
  IoContext io;

  constexpr int k_background = 20;
  std::vector<std::unique_ptr<Signal>> background_signals;
  std::atomic<int> background_wakes = 0;
  ex::counting_scope background;
  for (int i = 0; i < k_background; ++i) {
    background_signals.push_back(std::make_unique<Signal>());
    const Signal& signal = *background_signals.back();
    io.spawn_watch(signal.handle(), background,
                   [&signal, &background_wakes]() noexcept {
                     signal.reset();
                     ++background_wakes;
                   });
  }
  std::atomic<bool> done = false;
  std::thread background_setter([&] {
    std::mt19937 rng(1234);
    while (!done) {
      background_signals[rng() % k_background]->set();
      jitter(rng);
    }
  });

  constexpr int k_threads = 8;
  constexpr int k_iterations = 300;
  std::atomic<int> wrong = 0;
  std::vector<std::thread> workers;
  for (int t = 0; t < k_threads; ++t) {
    workers.emplace_back([&io, &wrong, t] {
      std::mt19937 rng(t);
      for (int i = 0; i < k_iterations; ++i) {
        const bool before_start = rng() % 8 == 0;
        // How many callbacks before the callback requests a stop itself, or 0
        // for never. A stop requested from outside may race it and win, in
        // which case the callback may be called again; one the callback's own
        // request made must stop it being called again.
        const int stop_after =
            rng() % 2 == 0 ? 0 : 1 + static_cast<int>(rng() % 4);
        const int events = static_cast<int>(rng() % 6);

        const Signal signal;
        ex::inplace_stop_source source;
        std::atomic<int> wakes = 0;
        bool stopped_itself = false;  // Touched only by the callback.
        std::atomic<int> after_own_stop = 0;
        std::atomic<int> completions = 0;
        std::atomic<Outcome> outcome = Outcome::error;
        const auto record = [&](Outcome result) noexcept {
          outcome = result;
          ++completions;
        };

        if (before_start) {
          source.request_stop();
        }
        ex::counting_scope scope;
        ex::spawn(
            ex::write_env(io.watch(signal.handle(),
                                   [&]() noexcept {
                                     signal.reset();
                                     if (stopped_itself) {
                                       ++after_own_stop;
                                     }
                                     if (++wakes == stop_after) {
                                       stopped_itself = source.request_stop();
                                     }
                                   }),
                          ex::prop{ex::get_stop_token, source.get_token()}) |
                ex::then([&]() noexcept { record(Outcome::ready); }) |
                ex::upon_stopped([&]() noexcept { record(Outcome::stopped); }),
            scope.get_token());
        for (int e = 0; e < events; ++e) {
          signal.set();
          jitter(rng);
        }
        source.request_stop();
        ex::sync_wait(scope.join());

        if (completions != 1 || outcome != Outcome::stopped || wakes > events ||
            (before_start && wakes != 0) || after_own_stop != 0) {
          ++wrong;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(wrong.load(), 0);

  done = true;
  background_setter.join();
  background.request_stop();
  ex::sync_wait(background.join());
  EXPECT_GT(background_wakes.load(), 0);
}

}  // namespace
