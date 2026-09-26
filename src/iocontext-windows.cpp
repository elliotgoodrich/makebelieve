// SPDX-License-Identifier: MIT
#include "iocontextpoller.hpp"

#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

}  // namespace

// The handles being watched, and an auto-reset event that wake() signals,
// always waited on first.
struct IoContextPoller::State {
  HANDLE wakeup = nullptr;
  std::vector<std::pair<HANDLE, void*>> watched;

  State() : wakeup(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    if (wakeup == nullptr) {
      throw std::system_error(last_error_code(), "CreateEvent");
    }
  }

  ~State() { CloseHandle(wakeup); }

  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&&) = delete;
  State& operator=(State&&) = delete;
};

IoContextPoller::IoContextPoller() : m_state(std::make_unique<State>()) {}

IoContextPoller::~IoContextPoller() = default;

std::error_code IoContextPoller::add(NativeHandle handle, void* key) {
  // One slot goes to the wake-up event.
  if (m_state->watched.size() + 1 >= MAXIMUM_WAIT_OBJECTS) {
    return std::make_error_code(std::errc::too_many_files_open);
  }
  m_state->watched.emplace_back(handle, key);
  return {};
}

void IoContextPoller::remove(NativeHandle /*handle*/, void* key) noexcept {
  std::erase_if(m_state->watched,
                [key](const auto& entry) { return entry.second == key; });
}

void IoContextPoller::wait(std::vector<Ready>& ready) {
  std::vector<HANDLE> handles;
  handles.reserve(m_state->watched.size() + 1);
  handles.push_back(m_state->wakeup);
  for (const auto& [handle, key] : m_state->watched) {
    handles.push_back(handle);
  }

  const DWORD result = WaitForMultipleObjects(
      static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
  const auto count = static_cast<DWORD>(handles.size());
  if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
    ready.emplace_back(m_state->watched[result - WAIT_OBJECT_0 - 1].second,
                       std::error_code{});
  } else if (result > WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + count) {
    // An abandoned mutex is still acquired, so it counts as signalled.
    ready.emplace_back(m_state->watched[result - WAIT_ABANDONED_0 - 1].second,
                       std::error_code{});
  } else if (result == WAIT_FAILED) {
    // Most likely a handle closed while watched. Which one is not said, so
    // every wait fails rather than the loop spinning on the same error.
    const std::error_code error = last_error_code();
    for (const auto& [handle, key] : m_state->watched) {
      ready.emplace_back(key, error);
    }
  }
  // WAIT_OBJECT_0 is wake(), which needs nothing more.
}

void IoContextPoller::wake() noexcept {
  SetEvent(m_state->wakeup);
}

}  // namespace makebelieve
