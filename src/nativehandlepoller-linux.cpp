// SPDX-License-Identifier: MIT
#include "nativehandlepoller.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {errno, std::system_category()};
}

}  // namespace

// An epoll instance, and an eventfd registered with it under a null context
// that wake() writes to.
struct NativeHandlePoller::Impl {
  int epoll = -1;
  int wakeup = -1;

  Impl() {
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

  ~Impl() {
    ::close(wakeup);
    ::close(epoll);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
};

NativeHandlePoller::NativeHandlePoller() : m_impl(std::make_unique<Impl>()) {}

NativeHandlePoller::~NativeHandlePoller() = default;

// The registration is the descriptor itself, which is all epoll removes by.
NativeHandlePoller::Token NativeHandlePoller::add(NativeHandle handle,
                                                  void* context) {
  // Level-triggered: a descriptor the caller has not drained is reported
  // again, which is what a fresh wait on it should see.
  epoll_event event{.events = EPOLLIN, .data = {.ptr = context}};
  if (::epoll_ctl(m_impl->epoll, EPOLL_CTL_ADD, handle, &event) != 0) {
    throw std::system_error(last_error_code(), "epoll_ctl");
  }
  return static_cast<Token>(handle);
}

// Nothing to do: a level-triggered descriptor stays watched, and is reported
// by every wait while it is ready. `EPOLLONESHOT`, re-armed here with
// `EPOLL_CTL_MOD`, would cost a system call per readiness and change nothing,
// as re-arming a descriptor that is still ready reports it again at once.
void NativeHandlePoller::rearm(Token /*token*/) {}

void NativeHandlePoller::remove(Token token) noexcept {
  ::epoll_ctl(m_impl->epoll, EPOLL_CTL_DEL, static_cast<int>(token), nullptr);
}

std::span<void*> NativeHandlePoller::wait(
    std::span<void*, max_ready> buffer) noexcept {
  std::array<epoll_event, max_ready> events{};
  std::size_t written = 0;
  const int count = ::epoll_wait(m_impl->epoll, events.data(),
                                 static_cast<int>(events.size()), -1);
  // EINTR, or a failure there is nothing to do about but try again.
  for (int i = 0; i < count; ++i) {
    void* const context = events[static_cast<std::size_t>(i)].data.ptr;
    if (context == nullptr) {
      std::uint64_t discarded = 0;
      // A cast does not silence warn_unused_result on GCC, so the result is
      // bound and discarded: a failed read only means another wake-up.
      const ssize_t drained =
          ::read(m_impl->wakeup, &discarded, sizeof(discarded));
      static_cast<void>(drained);
    } else {
      // A hang-up or error counts as ready: the caller's read reports it.
      buffer[written++] = context;
    }
  }
  return buffer.first(written);
}

void NativeHandlePoller::wake() noexcept {
  const std::uint64_t one = 1;
  // Only a counter at its maximum fails, and that is already a wake-up.
  const ssize_t written = ::write(m_impl->wakeup, &one, sizeof(one));
  static_cast<void>(written);
}

}  // namespace makebelieve
