// SPDX-License-Identifier: MIT
#include "nativehandlepoller.hpp"

#include <array>
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

// The wait completion packet routines, which have no import library or
// header, so they are found in ntdll at run time. See
// https://learn.microsoft.com/windows/win32/devnotes/ntassociatewaitcompletionpacket
using NtStatus = LONG;
constexpr NtStatus k_status_success = 0;

using NtCreateWaitCompletionPacketFn = NtStatus(NTAPI*)(HANDLE* packet,
                                                        ACCESS_MASK access,
                                                        void* attributes);
using NtAssociateWaitCompletionPacketFn =
    NtStatus(NTAPI*)(HANDLE packet,
                     HANDLE port,
                     HANDLE target,
                     void* key,
                     void* apc_context,
                     NtStatus status,
                     ULONG_PTR information,
                     BOOLEAN* already_signalled);
using NtCancelWaitCompletionPacketFn =
    NtStatus(NTAPI*)(HANDLE packet, BOOLEAN remove_signalled_packet);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NtStatus status);

// Looks up @a name in ntdll, which every process has loaded.
template <class Fn>
Fn ntdll_function(const char* name) {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto function = reinterpret_cast<Fn>(
      ntdll == nullptr ? nullptr : GetProcAddress(ntdll, name));
  if (function == nullptr) {
    throw std::system_error(last_error_code(), name);
  }
  return function;
}

// One handle's watch: a wait completion packet, which the kernel queues to
// the port, with the watch as its key, once the handle is signalled. Its
// address is the registration `add` hands out. An association fires once,
// which suits a handle that stays signalled - a manual-reset event - as it is
// reported once rather than over and over, and associating it again is all
// `rearm` needs.
struct RegisteredWait {
  HANDLE handle;
  void* context;
  HANDLE packet = nullptr;

  // On the context's thread: whether the packet is associated and not yet
  // dequeued - waiting on the handle, on its way to the port, or there - and
  // whether remove() has left the watch for wait() to free, as its packet
  // could not be taken back out of the port. Closing the packet takes back
  // one that is queued, so a watch left for wait() keeps it open.
  bool associated = false;
  bool removed = false;

  RegisteredWait(HANDLE handle, void* context) noexcept
      : handle(handle), context(context) {}

  ~RegisteredWait() {
    if (packet != nullptr) {
      CloseHandle(packet);
    }
  }

  RegisteredWait(const RegisteredWait&) = delete;
  RegisteredWait& operator=(const RegisteredWait&) = delete;
  RegisteredWait(RegisteredWait&&) = delete;
  RegisteredWait& operator=(RegisteredWait&&) = delete;
};

}  // namespace

// An I/O completion port, which is what the context's thread blocks on, and
// that the kernel queues each watch's packet to once its handle is signalled,
// with no thread in between, so there is no limit on how many are watched at
// once. A completion with a null key is wake().
struct NativeHandlePoller::Impl {
  NtCreateWaitCompletionPacketFn create_packet =
      ntdll_function<NtCreateWaitCompletionPacketFn>(
          "NtCreateWaitCompletionPacket");
  NtAssociateWaitCompletionPacketFn associate_packet =
      ntdll_function<NtAssociateWaitCompletionPacketFn>(
          "NtAssociateWaitCompletionPacket");
  NtCancelWaitCompletionPacketFn cancel_packet =
      ntdll_function<NtCancelWaitCompletionPacketFn>(
          "NtCancelWaitCompletionPacket");
  RtlNtStatusToDosErrorFn status_to_error =
      ntdll_function<RtlNtStatusToDosErrorFn>("RtlNtStatusToDosError");
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

  [[nodiscard]] std::error_code error_code(NtStatus status) const {
    return {static_cast<int>(status_to_error(status)), std::system_category()};
  }

  // Asks the kernel to queue @a registered's packet once its handle is
  // signalled - at once if it already is.
  void associate(RegisteredWait& registered) const {
    const NtStatus status =
        associate_packet(registered.packet, port, registered.handle,
                         &registered, nullptr, k_status_success, 0, nullptr);
    if (status != k_status_success) {
      throw std::system_error(error_code(status),
                              "NtAssociateWaitCompletionPacket");
    }
    registered.associated = true;
  }
};

NativeHandlePoller::NativeHandlePoller() : m_impl(std::make_unique<Impl>()) {}

NativeHandlePoller::~NativeHandlePoller() = default;

NativeHandlePoller::Token NativeHandlePoller::add(NativeHandle handle,
                                                  void* context) {
  auto registered = std::make_unique<RegisteredWait>(handle, context);
  const NtStatus status =
      m_impl->create_packet(&registered->packet, GENERIC_ALL, nullptr);
  if (status != k_status_success) {
    registered->packet = nullptr;
    throw std::system_error(m_impl->error_code(status),
                            "NtCreateWaitCompletionPacket");
  }
  m_impl->associate(*registered);
  return static_cast<Token>(
      reinterpret_cast<std::uintptr_t>(registered.release()));
}

// wait() has dequeued the packet, so it is free to associate again. If that
// fails, the watch is left unassociated, for remove() to free at once.
void NativeHandlePoller::rearm(Token token) {
  m_impl->associate(
      *reinterpret_cast<RegisteredWait*>(static_cast<std::uintptr_t>(token)));
}

// Success means that nothing reaches the port: the wait is cancelled before
// it was satisfied, or its packet is taken back out of the port. Otherwise a
// packet still associated is on its way to the port, or there and not taken
// back - the kernel reports "pending" or "already completed" - so the watch is
// left for wait() to dequeue and free. One not associated - never, or not
// since wait() dequeued its packet - has nothing to come.
void NativeHandlePoller::remove(Token token) noexcept {
  auto* const registered =
      reinterpret_cast<RegisteredWait*>(static_cast<std::uintptr_t>(token));
  if (m_impl->cancel_packet(registered->packet, TRUE) != k_status_success &&
      registered->associated) {
    registered->removed = true;
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
    registered->associated = false;
    if (registered->removed) {
      delete registered;
    } else {
      buffer[written++] = registered->context;
    }
  }
  return buffer.first(written);
}

void NativeHandlePoller::wake() noexcept {
  PostQueuedCompletionStatus(m_impl->port, 0, 0, nullptr);
}

}  // namespace makebelieve
