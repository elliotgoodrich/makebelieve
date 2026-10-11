// SPDX-License-Identifier: MIT
#include "iocontext.hpp"

#include <stdexec/execution.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace {

using namespace makebelieve;

// A handle the benchmark can make ready and unready: an eventfd on Linux, a
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

// Waits on @a count handles that never become ready, so that the context has
// that many other waits registered, until destroyed.
class OtherWaits {
  std::vector<std::unique_ptr<Signal>> m_signals;
  stdexec::counting_scope m_scope;

 public:
  OtherWaits(IoContext& io, int count) {
    for (int i = 0; i < count; ++i) {
      m_signals.push_back(std::make_unique<Signal>());
      stdexec::spawn(io.async_wait(m_signals.back()->handle()) |
                         stdexec::upon_error([](std::error_code) noexcept {}),
                     m_scope.get_token());
    }
  }

  ~OtherWaits() {
    m_scope.request_stop();
    stdexec::sync_wait(m_scope.join());
  }

  OtherWaits(const OtherWaits&) = delete;
  OtherWaits& operator=(const OtherWaits&) = delete;
  OtherWaits(OtherWaits&&) = delete;
  OtherWaits& operator=(OtherWaits&&) = delete;
};

// Counts the events a watch has handled, for the benchmark's thread to wait
// on.
class Handled {
  std::atomic<std::uint64_t> m_count = 0;

 public:
  void add() noexcept {
    m_count.fetch_add(1, std::memory_order_release);
    m_count.notify_one();
  }

  // Waits until more than @a seen events have been handled, returning how
  // many have.
  std::uint64_t wait_past(std::uint64_t seen) noexcept {
    m_count.wait(seen, std::memory_order_acquire);
    return m_count.load(std::memory_order_acquire);
  }
};

// One event per iteration: the benchmark's thread makes the handle ready and
// waits for a `spawn_watch` to handle it, with range(0) other waits registered
// throughout.
void watch_round_trip(benchmark::State& state) {
  IoContext io;
  const OtherWaits others(io, static_cast<int>(state.range(0)));
  const Signal signal;
  Handled handled;
  stdexec::counting_scope scope;
  io.spawn_watch(signal.handle(), scope, [&]() noexcept {
    signal.reset();
    handled.add();
  });

  std::uint64_t seen = 0;
  for (auto _ : state) {
    signal.set();
    seen = handled.wait_past(seen);
  }
  state.SetItemsProcessed(state.iterations());

  scope.request_stop();
  stdexec::sync_wait(scope.join());
}
BENCHMARK(watch_round_trip)
    ->ArgName("others")
    ->Arg(0)
    ->Arg(10)
    ->Arg(100)
    ->UseRealTime();

// The baseline for `watch_round_trip`: a wait per event, made by hand from the
// benchmark's thread. The handle is made ready first, as a wait started after
// it blocks this thread, so each wait finds it ready as soon as it registers.
void async_wait_round_trip(benchmark::State& state) {
  IoContext io;
  const OtherWaits others(io, static_cast<int>(state.range(0)));
  const Signal signal;

  for (auto _ : state) {
    signal.set();
    stdexec::sync_wait(io.async_wait(signal.handle()) |
                       stdexec::upon_error([](std::error_code) noexcept {}));
    signal.reset();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(async_wait_round_trip)
    ->ArgName("others")
    ->Arg(0)
    ->Arg(10)
    ->Arg(100)
    ->UseRealTime();

// range(0) events per iteration, all but the first raised from inside the
// watch's callback, so the context's thread handles them back to back with no
// other thread involved.
void watch_burst(benchmark::State& state) {
  IoContext io;
  const Signal signal;
  const std::int64_t burst = state.range(0);
  std::int64_t remaining = 0;  // Touched only by the callback.
  Handled handled;
  stdexec::counting_scope scope;
  io.spawn_watch(signal.handle(), scope, [&]() noexcept {
    signal.reset();
    if (--remaining > 0) {
      signal.set();
    } else {
      handled.add();
    }
  });

  std::uint64_t seen = 0;
  for (auto _ : state) {
    // The callback last ran before `handled.add`, which this thread has seen,
    // and runs next only after `set`, so it does not race this write.
    remaining = burst;
    signal.set();
    seen = handled.wait_past(seen);
  }
  state.SetItemsProcessed(state.iterations() * burst);

  scope.request_stop();
  stdexec::sync_wait(scope.join());
}
BENCHMARK(watch_burst)->ArgName("events")->Arg(64)->UseRealTime();

}  // namespace
