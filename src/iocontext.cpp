// SPDX-License-Identifier: MIT
#include "iocontext.hpp"

#include "tracer.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <functional>
#include <system_error>
#include <utility>

namespace makebelieve {

// Befriends the constructor alone, which makes one in the thread it starts and
// hands it to `run`. Befriending a member needs `IoContext` complete, so it is
// defined here rather than where it is declared. Neither copyable nor
// addressable, so it reaches a function only by reference from a caller that
// has one, and is not easily kept past that call.
class detail::IoThreadPasskey {
  IoThreadPasskey() = default;
  friend IoContext::IoContext();

 public:
  IoThreadPasskey(const IoThreadPasskey&) = delete;
  IoThreadPasskey& operator=(const IoThreadPasskey&) = delete;
  IoThreadPasskey(IoThreadPasskey&&) = delete;
  IoThreadPasskey& operator=(IoThreadPasskey&&) = delete;
  ~IoThreadPasskey() = default;

  void operator&() const = delete;
};

#ifndef NDEBUG
// Not null, as the oldest task in the queue has a null `next`, but the address
// of an object of our own, which no task can share; it is only ever compared,
// never followed. Not `const`, so that a linker folding identical constants
// cannot give it another's address.
detail::IoContextTask* detail::IoContextTask::not_queued() noexcept {
  static constinit char marker = 0;
  return reinterpret_cast<IoContextTask*>(&marker);
}
#endif

IoContext::IoContext() : m_thread([this] { run(detail::IoThreadPasskey{}); }) {}

IoContext::~IoContext() {
  m_stopping.store(true, std::memory_order_release);
  m_poller.wake();
}

// Defined out of line, so its callers instantiate none of the sender.
void IoContext::spawn_watch(
    NativeHandle handle,
    stdexec::counting_scope& scope,
    std::move_only_function<void() noexcept> on_ready,
    std::move_only_function<void(std::error_code) noexcept> on_error) {
  stdexec::spawn(
      watch(handle, std::move(on_ready)) |
          stdexec::upon_error([this, on_error = std::move(on_error)](
                                  std::error_code error) mutable noexcept {
            on_error(error);
            m_watch_failed.request_stop();
          }),
      scope.get_token());
}

std::stop_token IoContext::watch_failure_token() const noexcept {
  return m_watch_failed.get_token();
}

// The context's thread takes the whole stack at once, so only a push onto an
// empty one wakes it: a push onto a nonempty one has a wake on its way already.
// The old head is kept in a local, as @a task may be run and freed as soon as
// it is pushed.
void IoContext::submit(detail::IoContextTask& task) noexcept {
#ifndef NDEBUG
  assert(task.next == detail::IoContextTask::not_queued() &&
         "IoContextTask queued twice");
#endif
  detail::IoContextTask* head = m_tasks.load(std::memory_order_relaxed);
  do {
    task.next = head;
  } while (!m_tasks.compare_exchange_weak(
      head, &task, std::memory_order_release, std::memory_order_relaxed));
  if (head == nullptr) {
    m_poller.wake();
  }
}

// The stop callback runs at most once, so @a wait is pushed at most once, and
// `finish` waits it out before the wait can be freed, so @a wait outlives
// this. Wakes the context's thread only on a push onto an empty stack, as
// `submit` does.
void IoContext::cancel(detail::IoContextWait& wait) noexcept {
  detail::IoContextWait* head = m_cancels.load(std::memory_order_relaxed);
  do {
    wait.next_cancel = head;
  } while (!m_cancels.compare_exchange_weak(
      head, &wait, std::memory_order_release, std::memory_order_relaxed));
  if (head == nullptr) {
    m_poller.wake();
  }
}

void IoContext::begin_wait(const detail::IoThreadPasskey& passkey,
                           detail::IoContextWait& wait) noexcept {
  wait.arm(passkey);
  try {
    wait.registration = m_poller.add(wait.handle, &wait);
    LinkedNode<detail::IoContextWait>::insert_before(&m_registered, &wait);
  } catch (const std::system_error& e) {
    finish(passkey, wait, e.code(), false);
  } catch (...) {
    finish(passkey, wait, std::make_error_code(std::errc::not_enough_memory),
           false);
  }
}

void IoContext::run(const detail::IoThreadPasskey& passkey) noexcept {
  MB_TRACE_THREAD_NAME("io");
  std::array<void*, NativeHandlePoller::max_ready> ready{};
  while (true) {
    const bool stopping = m_stopping.load(std::memory_order_acquire);

    // The queue holds the newest task first, so reverse it to run them in the
    // order they were submitted.
    detail::IoContextTask* tasks = nullptr;
    detail::IoContextTask* pushed =
        m_tasks.exchange(nullptr, std::memory_order_acquire);
    while (pushed != nullptr) {
      detail::IoContextTask* const task = std::exchange(pushed, pushed->next);
      task->next = tasks;
      tasks = task;
    }

    // Read each link, and in Debug builds mark the task as no longer queued,
    // before running it: running it may complete and free it.
    while (tasks != nullptr) {
      detail::IoContextTask* const task = std::exchange(tasks, tasks->next);
#ifndef NDEBUG
      task->next = detail::IoContextTask::not_queued();
#endif
      task->execute(passkey);
    }

    // Finishing a wait takes any cancellations pushed meanwhile, so they are
    // stopped on this pass too. One pushed after the last of those wakes the
    // poller, and is stopped on the next pass.
    take_cancels(passkey);
    while (!m_cancelled.alone()) {
      detail::IoContextWait* const wait = m_cancelled.next->get();
      unregister(passkey, *wait);
      finish(passkey, *wait, {}, true);
    }

    if (stopping) {
      assert(m_registered.alone() && "IoContext destroyed while still waiting");
      return;
    }

    // Each wait is reported at most once, and none can be freed by another's
    // completion before it has completed itself, nor by a watch's callback,
    // which completes nothing, so each is still registered - though finishing
    // one may have moved another into `m_cancelled`. A watch stays registered
    // once its callback returns, and is rearmed even if a stop has been
    // requested meanwhile - perhaps by the callback itself, which only pushes
    // the watch onto `m_cancels` - as the next pass unregisters it. A watch
    // that cannot be rearmed has failed, and is finished as one that could not
    // be registered is.
    for (void* const context : m_poller.wait(ready)) {
      auto* const wait = static_cast<detail::IoContextWait*>(context);
      if (wait->handle_ready(passkey)) {
        unregister(passkey, *wait);
        finish(passkey, *wait, {}, false);
      } else {
        try {
          m_poller.rearm(wait->registration);
        } catch (const std::system_error& e) {
          unregister(passkey, *wait);
          finish(passkey, *wait, e.code(), false);
        }
      }
    }
  }
}

// A wait is pushed only by its stop callback, which it disarms before it is
// finished, so every wait on `m_cancels` is alive. It is in `m_registered`, or
// in no list if registering it failed, where removing it does nothing.
void IoContext::take_cancels(
    const detail::IoThreadPasskey& /*passkey*/) noexcept {
  detail::IoContextWait* wait =
      m_cancels.exchange(nullptr, std::memory_order_acquire);
  while (wait != nullptr) {
    detail::IoContextWait* const next = wait->next_cancel;
    LinkedNode<detail::IoContextWait>::remove(wait);
    LinkedNode<detail::IoContextWait>::insert_before(&m_cancelled, wait);
    wait = next;
  }
}

void IoContext::unregister(const detail::IoThreadPasskey& /*passkey*/,
                           detail::IoContextWait& wait) noexcept {
  m_poller.remove(wait.registration);
}

// Disarming waits out a stop callback running on another thread, so none can
// push @a wait after it. Taking the cancellations then leaves @a wait in one of
// the lists, not on `m_cancels`, so it can be removed before it completes and
// is freed.
void IoContext::finish(const detail::IoThreadPasskey& passkey,
                       detail::IoContextWait& wait,
                       std::error_code error,
                       bool stopped) noexcept {
  wait.disarm(passkey);
  take_cancels(passkey);
  LinkedNode<detail::IoContextWait>::remove(&wait);
  wait.complete(passkey, error, stopped);
}

}  // namespace makebelieve
