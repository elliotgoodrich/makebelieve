// SPDX-License-Identifier: MIT
#include "nativehandlepoller.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <thread>
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

// A handle that stays ready once set: an eventfd on Linux, a manual-reset
// event on Windows.
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

  [[nodiscard]] NativeHandle handle() const { return m_handle; }

  void set() const {
#ifdef _WIN32
    SetEvent(m_handle);
#else
    const std::uint64_t one = 1;
    const ssize_t written = ::write(m_handle, &one, sizeof(one));
    static_cast<void>(written);
#endif
  }
};

int count_of(const std::vector<void*>& ready, void* context) {
  return static_cast<int>(std::ranges::count(ready, context));
}

// Waits until @a context is reported, as a wait may return with nothing.
void wait_for(NativeHandlePoller& poller, void* context) {
  std::vector<void*> ready;
  while (count_of(ready, context) == 0) {
    poller.wait(ready);
  }
}

TEST(NativeHandlePoller, ReportsAReadyHandleUnderItsContext) {
  NativeHandlePoller poller;
  const Signal signal;
  int context = 0;
  signal.set();
  const auto registration = poller.add(signal.handle(), &context);
  wait_for(poller, &context);
  poller.remove(registration);
}

TEST(NativeHandlePoller, WakeReturnsAWaitWithNothingReady) {
  NativeHandlePoller poller;
  const Signal signal;
  int context = 0;
  const auto registration = poller.add(signal.handle(), &context);
  poller.wake();
  std::vector<void*> ready;
  poller.wait(ready);
  EXPECT_TRUE(ready.empty());
  poller.remove(registration);
}

TEST(NativeHandlePoller, AHandleRemovedOnceReadyIsNotReported) {
  NativeHandlePoller poller;
  const Signal signal;
  int context = 0;
  signal.set();
  const auto registration = poller.add(signal.handle(), &context);
  // Long enough for Windows to have posted the readiness to the port.
  std::this_thread::sleep_for(50ms);
  poller.remove(registration);

  poller.wake();
  std::vector<void*> ready;
  poller.wait(ready);
  EXPECT_EQ(count_of(ready, &context), 0);
}

TEST(NativeHandlePoller, AKeyAddedAgainAfterARemovalIsReportedOnce) {
  NativeHandlePoller poller;
  const Signal signal;
  int context = 0;
  signal.set();
  const auto first = poller.add(signal.handle(), &context);
  std::this_thread::sleep_for(50ms);
  poller.remove(first);

  // What the first registration posted must not count for the second.
  const auto second = poller.add(signal.handle(), &context);
  std::this_thread::sleep_for(50ms);
  std::vector<void*> ready;
  while (count_of(ready, &context) == 0) {
    poller.wait(ready);
  }
  EXPECT_EQ(count_of(ready, &context), 1);
  poller.remove(second);
}

TEST(NativeHandlePoller, IsDestroyedWithAReadinessStillUnread) {
  const Signal signal;
  int context = 0;
  signal.set();
  NativeHandlePoller poller;
  const auto registration = poller.add(signal.handle(), &context);
  std::this_thread::sleep_for(50ms);
  poller.remove(registration);
  // Destroyed without a wait, leaving what was posted for it to free.
}

TEST(NativeHandlePoller, AddingAnUnwaitableHandleThrows) {
  NativeHandlePoller poller;
  int context = 0;
#ifdef _WIN32
  const NativeHandle bad = nullptr;
#else
  const NativeHandle bad = -1;
#endif
  EXPECT_THROW(static_cast<void>(poller.add(bad, &context)), std::system_error);
}

}  // namespace
