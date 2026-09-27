// SPDX-License-Identifier: MIT
#include "iocontextpoller.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

// What a handle's wait posts to the port once it is signalled: the key it was
// added under and which registration of that key it was, so a completion
// posted just before the wait was removed cannot be mistaken for one from a
// later wait that reuses the key.
struct Registration {
  HANDLE port;
  void* key;
  DWORD generation;
  HANDLE wait = nullptr;
};

// Runs on one of the system's wait threads, so it only posts.
void CALLBACK on_signalled(void* context, BOOLEAN /*timed_out*/) {
  const auto* registration = static_cast<const Registration*>(context);
  PostQueuedCompletionStatus(registration->port, registration->generation,
                             reinterpret_cast<ULONG_PTR>(registration->key),
                             nullptr);
}

}  // namespace

// An I/O completion port, which is what the context's thread blocks on, and
// the handle waits posting to it: the system's wait threads watch the handles
// themselves, so there is no limit on how many are watched at once. A
// completion with a null key is wake().
struct IoContextPoller::State {
  HANDLE port = nullptr;
  DWORD next_generation = 0;

  // Keyed by what each handle was added under. Heap-allocated, as each wait
  // holds its registration's address.
  std::unordered_map<void*, std::unique_ptr<Registration>> registrations;

  State() : port(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)) {
    if (port == nullptr) {
      throw std::system_error(last_error_code(), "CreateIoCompletionPort");
    }
  }

  ~State() { CloseHandle(port); }

  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&&) = delete;
  State& operator=(State&&) = delete;
};

IoContextPoller::IoContextPoller() : m_state(std::make_unique<State>()) {}

IoContextPoller::~IoContextPoller() = default;

std::error_code IoContextPoller::add(NativeHandle handle, void* key) {
  auto registration = std::make_unique<Registration>(
      Registration{.port = m_state->port,
                   .key = key,
                   .generation = ++m_state->next_generation});
  // Once only: a signalled handle is reported once, as a wait on it would, and
  // is watched again only if it is added again.
  if (!RegisterWaitForSingleObject(
          &registration->wait, handle, &on_signalled, registration.get(),
          INFINITE, WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD)) {
    return last_error_code();
  }
  m_state->registrations.insert_or_assign(key, std::move(registration));
  return {};
}

void IoContextPoller::remove(NativeHandle /*handle*/, void* key) noexcept {
  const auto it = m_state->registrations.find(key);
  if (it == m_state->registrations.end()) {
    return;
  }
  // Blocks until a callback already running has posted, so the registration
  // outlives any use of it. What it posted is dropped by wait().
  UnregisterWaitEx(it->second->wait, INVALID_HANDLE_VALUE);
  m_state->registrations.erase(it);
}

void IoContextPoller::wait(std::vector<Ready>& ready) {
  std::array<OVERLAPPED_ENTRY, 64> entries{};
  ULONG count = 0;
  if (!GetQueuedCompletionStatusEx(m_state->port, entries.data(),
                                   static_cast<ULONG>(entries.size()), &count,
                                   INFINITE, FALSE)) {
    return;  // Nothing there is to be done about but try again.
  }
  for (ULONG i = 0; i < count; ++i) {
    const OVERLAPPED_ENTRY& entry = entries[i];
    void* const key = reinterpret_cast<void*>(entry.lpCompletionKey);
    if (key == nullptr) {
      continue;  // wake()
    }
    const auto it = m_state->registrations.find(key);
    if (it != m_state->registrations.end() &&
        it->second->generation == entry.dwNumberOfBytesTransferred) {
      ready.emplace_back(key, std::error_code{});
    }
  }
}

void IoContextPoller::wake() noexcept {
  PostQueuedCompletionStatus(m_state->port, 0, 0, nullptr);
}

}  // namespace makebelieve
