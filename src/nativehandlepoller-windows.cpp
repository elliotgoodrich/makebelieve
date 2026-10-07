// SPDX-License-Identifier: MIT
#include "nativehandlepoller.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

#include <windows.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

// One handle's wait, which posts itself to the port, as the completion key,
// once the handle is signalled. Its address is the registration `add` hands
// out. A packet cannot be taken back out of the port, so one posted just
// before the wait was removed keeps it alive until wait() dequeues it.
struct RegisteredWait {
  HANDLE port;
  void* context;
  HANDLE wait = nullptr;

  // Set on the system's wait thread just before posting, which happens at
  // most once.
  std::atomic<bool> posted = false;

  // On the context's thread: whether wait() has dequeued the packet, and
  // whether remove() has left the wait for wait() to free.
  bool consumed = false;
  bool removed = false;

  RegisteredWait(HANDLE port, void* context) noexcept
      : port(port), context(context) {}
};

// Runs on one of the system's wait threads, so it only posts.
void CALLBACK on_signalled(void* context, BOOLEAN /*timed_out*/) {
  auto* const registered = static_cast<RegisteredWait*>(context);
  registered->posted.store(true, std::memory_order_release);
  PostQueuedCompletionStatus(registered->port, 0,
                             reinterpret_cast<ULONG_PTR>(registered), nullptr);
}

}  // namespace

// An I/O completion port, which is what the context's thread blocks on, and
// the handle waits posting to it: the system's wait threads watch the handles
// themselves, so there is no limit on how many are watched at once. A
// completion with a null key is wake().
struct NativeHandlePoller::Impl {
  HANDLE port = nullptr;

  Impl() : port(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)) {
    if (port == nullptr) {
      throw std::system_error(last_error_code(), "CreateIoCompletionPort");
    }
  }

  // Frees the waits still waiting for their packets to be dequeued.
  ~Impl() {
    std::array<OVERLAPPED_ENTRY, 64> entries{};
    ULONG count = 0;
    while (GetQueuedCompletionStatusEx(port, entries.data(),
                                       static_cast<ULONG>(entries.size()),
                                       &count, 0, FALSE)) {
      for (ULONG i = 0; i < count; ++i) {
        // Only removed waits can be left: all others have been removed first.
        delete reinterpret_cast<RegisteredWait*>(entries[i].lpCompletionKey);
      }
    }
    CloseHandle(port);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
};

NativeHandlePoller::NativeHandlePoller() : m_impl(std::make_unique<Impl>()) {}

NativeHandlePoller::~NativeHandlePoller() = default;

NativeHandlePoller::Token NativeHandlePoller::add(NativeHandle handle,
                                                  void* context) {
  auto registered = std::make_unique<RegisteredWait>(m_impl->port, context);
  // Once only: a signalled handle is reported once, as a wait on it would, and
  // is watched again only if it is added again.
  if (!RegisterWaitForSingleObject(
          &registered->wait, handle, &on_signalled, registered.get(), INFINITE,
          WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD)) {
    throw std::system_error(last_error_code(), "RegisterWaitForSingleObject");
  }
  return static_cast<Token>(
      reinterpret_cast<std::uintptr_t>(registered.release()));
}

void NativeHandlePoller::remove(Token token) noexcept {
  auto* const registered =
      reinterpret_cast<RegisteredWait*>(static_cast<std::uintptr_t>(token));
  // Blocks until a callback already running has posted, so nothing touches
  // the wait from the system's side once this returns.
  UnregisterWaitEx(registered->wait, INVALID_HANDLE_VALUE);
  if (registered->posted.load(std::memory_order_acquire) &&
      !registered->consumed) {
    registered->removed = true;  // wait() frees it with its packet.
  } else {
    delete registered;
  }
}

void NativeHandlePoller::wait(std::vector<void*>& ready) {
  std::array<OVERLAPPED_ENTRY, 64> entries{};
  ULONG count = 0;
  if (!GetQueuedCompletionStatusEx(m_impl->port, entries.data(),
                                   static_cast<ULONG>(entries.size()), &count,
                                   INFINITE, FALSE)) {
    return;  // Nothing there is to be done about but try again.
  }
  for (ULONG i = 0; i < count; ++i) {
    auto* const registered =
        reinterpret_cast<RegisteredWait*>(entries[i].lpCompletionKey);
    if (registered == nullptr) {
      continue;  // wake()
    }
    if (registered->removed) {
      delete registered;
    } else {
      registered->consumed = true;
      ready.push_back(registered->context);
    }
  }
}

void NativeHandlePoller::wake() noexcept {
  PostQueuedCompletionStatus(m_impl->port, 0, 0, nullptr);
}

}  // namespace makebelieve
