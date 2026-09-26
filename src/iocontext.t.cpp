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

std::thread::id context_thread(IoContext& io) {
  return std::get<0>(
      ex::sync_wait(ex::schedule(io.get_scheduler()) |
                    ex::then([] { return std::this_thread::get_id(); }))
          .value());
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
  EXPECT_EQ(wait_on(io, signal, stop.get_token()), Outcome::stopped);
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

}  // namespace
