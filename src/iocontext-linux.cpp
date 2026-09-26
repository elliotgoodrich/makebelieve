// SPDX-License-Identifier: MIT
#include "iocontextpoller.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {errno, std::system_category()};
}

}  // namespace

// An epoll instance, and an eventfd registered with it under a null key that
// wake() writes to.
struct IoContextPoller::State {
  int epoll = -1;
  int wakeup = -1;

  State() {
    epoll = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll < 0) {
      throw std::system_error(last_error_code(), "epoll_create1");
    }
    wakeup = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeup < 0) {
      const std::error_code error = last_error_code();
      ::close(epoll);
      throw std::system_error(error, "eventfd");
    }
    epoll_event event{.events = EPOLLIN, .data = {.ptr = nullptr}};
    if (::epoll_ctl(epoll, EPOLL_CTL_ADD, wakeup, &event) != 0) {
      const std::error_code error = last_error_code();
      ::close(wakeup);
      ::close(epoll);
      throw std::system_error(error, "epoll_ctl");
    }
  }

  ~State() {
    ::close(wakeup);
    ::close(epoll);
  }

  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&&) = delete;
  State& operator=(State&&) = delete;
};

IoContextPoller::IoContextPoller() : m_state(std::make_unique<State>()) {}

IoContextPoller::~IoContextPoller() = default;

std::error_code IoContextPoller::add(NativeHandle handle, void* key) {
  // Level-triggered: a descriptor the caller has not drained is reported
  // again, which is what a fresh wait on it should see.
  epoll_event event{.events = EPOLLIN, .data = {.ptr = key}};
  if (::epoll_ctl(m_state->epoll, EPOLL_CTL_ADD, handle, &event) != 0) {
    return last_error_code();
  }
  return {};
}

void IoContextPoller::remove(NativeHandle handle, void* /*key*/) noexcept {
  ::epoll_ctl(m_state->epoll, EPOLL_CTL_DEL, handle, nullptr);
}

void IoContextPoller::wait(std::vector<Ready>& ready) {
  std::array<epoll_event, 64> events{};
  const int count = ::epoll_wait(m_state->epoll, events.data(),
                                 static_cast<int>(events.size()), -1);
  // EINTR, or a failure there is nothing to do about but try again.
  for (int i = 0; i < count; ++i) {
    void* const key = events[static_cast<std::size_t>(i)].data.ptr;
    if (key == nullptr) {
      std::uint64_t discarded = 0;
      // A cast does not silence warn_unused_result on GCC, so the result is
      // bound and discarded: a failed read only means another wake-up.
      const ssize_t drained =
          ::read(m_state->wakeup, &discarded, sizeof(discarded));
      static_cast<void>(drained);
    } else {
      // A hang-up or error counts as ready: the caller's read reports it.
      ready.emplace_back(key, std::error_code{});
    }
  }
}

void IoContextPoller::wake() noexcept {
  const std::uint64_t one = 1;
  // Only a counter at its maximum fails, and that is already a wake-up.
  const ssize_t written = ::write(m_state->wakeup, &one, sizeof(one));
  static_cast<void>(written);
}

}  // namespace makebelieve
