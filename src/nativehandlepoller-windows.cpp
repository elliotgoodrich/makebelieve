// SPDX-License-Identifier: MIT
#include "nativehandlepoller.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>

#include <windows.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

// One handle's watch: a thread pool wait, which posts the watch to the port,
// as the completion key, once the handle is signalled. Its address is the
// registration `add` hands out. A thread pool wait is armed for one signal at
// a time, which suits a handle that stays signalled - a manual-reset event -
// as it is reported once rather than over and over, and setting it again is
// all `rearm` needs. A packet cannot be taken back out of the port, so one
// posted just before the watch was removed keeps it alive until wait()
// dequeues it.
struct RegisteredWait {
  HANDLE port;
  HANDLE handle;
  void* context;
  PTP_WAIT wait = nullptr;

  // Set on a thread pool thread just before posting, which happens at most
  // once per arming.
  std::atomic<bool> posted = false;

  // On the context's thread: whether wait() has dequeued the packet, and
  // whether remove() has left the watch for wait() to free.
  bool consumed = false;
  bool removed = false;

  RegisteredWait(HANDLE port, HANDLE handle, void* context) noexcept
      : port(port), handle(handle), context(context) {}
};

// Runs on a thread pool thread, so it only posts.
void CALLBACK on_signalled(PTP_CALLBACK_INSTANCE /*instance*/,
                           void* context,
                           PTP_WAIT /*wait*/,
                           TP_WAIT_RESULT /*result*/) {
  auto* const registered = static_cast<RegisteredWait*>(context);
  registered->posted.store(true, std::memory_order_release);
  PostQueuedCompletionStatus(registered->port, 0,
                             reinterpret_cast<ULONG_PTR>(registered), nullptr);
}

}  // namespace

// An I/O completion port, which is what the context's thread blocks on, and
// the thread pool waits posting to it: the thread pool watches the handles
// itself, so there is no limit on how many are watched at once. A completion
// with a null key is wake().
struct NativeHandlePoller::Impl {
  HANDLE port = nullptr;

  Impl() : port(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)) {
    if (port == nullptr) {
      throw std::system_error(last_error_code(), "CreateIoCompletionPort");
    }
  }

  // Frees the watches still waiting for their packets to be dequeued.
  ~Impl() {
    std::array<OVERLAPPED_ENTRY, 64> entries{};
    ULONG count = 0;
    while (GetQueuedCompletionStatusEx(port, entries.data(),
                                       static_cast<ULONG>(entries.size()),
                                       &count, 0, FALSE)) {
      for (ULONG i = 0; i < count; ++i) {
        // Only removed watches can be left: all others have been removed
        // first.
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
  // A thread pool wait takes any handle without complaint - and a null one as
  // a request to stop waiting - so a bad one is caught here instead. This
  // reads nothing of the handle's state, so it consumes no signal.
  DWORD flags = 0;
  if (!GetHandleInformation(handle, &flags)) {
    throw std::system_error(last_error_code(), "GetHandleInformation");
  }
  auto registered =
      std::make_unique<RegisteredWait>(m_impl->port, handle, context);
  registered->wait =
      CreateThreadpoolWait(&on_signalled, registered.get(), nullptr);
  if (registered->wait == nullptr) {
    throw std::system_error(last_error_code(), "CreateThreadpoolWait");
  }
  SetThreadpoolWait(registered->wait, handle, nullptr);
  return static_cast<Token>(
      reinterpret_cast<std::uintptr_t>(registered.release()));
}

// The packet has been dequeued, so the callback that posted it touches the
// watch no more, and no other is armed to: nothing races these writes, and
// arming publishes them to the next callback.
void NativeHandlePoller::rearm(Token token) noexcept {
  auto* const registered =
      reinterpret_cast<RegisteredWait*>(static_cast<std::uintptr_t>(token));
  registered->posted.store(false, std::memory_order_relaxed);
  registered->consumed = false;
  SetThreadpoolWait(registered->wait, registered->handle, nullptr);
}

void NativeHandlePoller::remove(Token token) noexcept {
  auto* const registered =
      reinterpret_cast<RegisteredWait*>(static_cast<std::uintptr_t>(token));
  // Stops the wait, cancels a callback queued but not yet started, and blocks
  // until one already running has posted, so nothing touches the watch from
  // the thread pool's side once the wait is closed.
  SetThreadpoolWait(registered->wait, nullptr, nullptr);
  WaitForThreadpoolWaitCallbacks(registered->wait, TRUE);
  CloseThreadpoolWait(registered->wait);
  if (registered->posted.load(std::memory_order_acquire) &&
      !registered->consumed) {
    registered->removed = true;  // wait() frees it with its packet.
  } else {
    delete registered;
  }
}

std::span<void*> NativeHandlePoller::wait(
    std::span<void*, max_ready> buffer) noexcept {
  std::array<OVERLAPPED_ENTRY, max_ready> entries{};
  std::size_t written = 0;
  ULONG count = 0;
  if (!GetQueuedCompletionStatusEx(m_impl->port, entries.data(),
                                   static_cast<ULONG>(entries.size()), &count,
                                   INFINITE, FALSE)) {
    return {};  // Nothing there is to be done about but try again.
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
      buffer[written++] = registered->context;
    }
  }
  return buffer.first(written);
}

void NativeHandlePoller::wake() noexcept {
  PostQueuedCompletionStatus(m_impl->port, 0, 0, nullptr);
}

}  // namespace makebelieve
