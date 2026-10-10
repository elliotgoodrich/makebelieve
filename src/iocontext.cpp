// SPDX-License-Identifier: MIT
#include "iocontext.hpp"

#include "tracer.hpp"

#include <cassert>
#include <cstddef>
#include <functional>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace makebelieve {

IoContext::IoContext() : m_thread([this] { run(); }) {}

IoContext::~IoContext() {
  {
    const std::lock_guard lock(m_mutex);
    m_stopping = true;
  }
  m_poller.wake();
}

void IoContext::spawn_watch(NativeHandle handle,
                            stdexec::counting_scope& scope,
                            std::move_only_function<void() noexcept> on_ready) {
  stdexec::spawn(watch(handle, std::move(on_ready)), scope.get_token());
}

void IoContext::submit(detail::IoContextTask& task) noexcept {
  assert(task.next == nullptr && "task already queued");
  {
    const std::lock_guard lock(m_mutex);
    task.next = nullptr;
    if (m_last_task == nullptr) {
      m_first_task = &task;
    } else {
      m_last_task->next = &task;
    }
    m_last_task = &task;
  }
  m_poller.wake();
}

void IoContext::cancel(detail::IoContextWait& wait) noexcept {
  {
    const std::lock_guard lock(m_mutex);
    if (wait.cancel_queued) {
      return;
    }
    wait.cancel_queued = true;
    wait.next_cancel = m_cancels;
    m_cancels = &wait;
  }
  m_poller.wake();
}

void IoContext::begin_wait(detail::IoContextWait& wait) noexcept {
  wait.arm();
  std::error_code error;
  try {
    wait.registration = m_poller.add(wait.handle, &wait);
    ++m_registered;
  } catch (const std::system_error& e) {
    error = e.code();
  } catch (...) {
    error = std::make_error_code(std::errc::not_enough_memory);
  }
  if (error) {
    finish(wait, error, false);
  }
}

void IoContext::run() {
  MB_TRACE_THREAD_NAME("io");
  std::vector<void*> ready;
  while (true) {
    detail::IoContextTask* tasks = nullptr;
    detail::IoContextWait* cancels = nullptr;
    bool stopping = false;
    {
      const std::lock_guard lock(m_mutex);
      tasks = std::exchange(m_first_task, nullptr);
      m_last_task = nullptr;
      cancels = std::exchange(m_cancels, nullptr);
      for (detail::IoContextWait* wait = cancels; wait != nullptr;
           wait = wait->next_cancel) {
        wait->cancel_queued = false;
      }
      stopping = m_stopping;
    }

    // Read each link before running the task, which may complete and free
    // it.
    while (tasks != nullptr) {
      detail::IoContextTask* const task = std::exchange(tasks, tasks->next);
      task->execute();
    }

    // A cancellation queued by a stop callback that fires while its wait is
    // armed above waits for the next pass; the poller has been woken for it.
    // Each wait here is still registered: finishing a wait first drops any
    // cancellation still queued for it.
    while (cancels != nullptr) {
      detail::IoContextWait* const wait =
          std::exchange(cancels, cancels->next_cancel);
      unregister(*wait);
      finish(*wait, {}, true);
    }

    if (stopping) {
      assert(m_registered == 0 && "IoContext destroyed while still waiting");
      return;
    }

    // Each wait is reported at most once, and none can be freed by another's
    // completion before it has completed itself, so each is still registered.
    ready.clear();
    m_poller.wait(ready);
    for (void* const context : ready) {
      auto* const wait = static_cast<detail::IoContextWait*>(context);
      unregister(*wait);
      finish(*wait, {}, false);
    }
  }
}

void IoContext::unregister(detail::IoContextWait& wait) noexcept {
  assert(m_registered > 0);
  --m_registered;
  m_poller.remove(wait.registration);
}

// The stop callback goes first, so none can queue a cancellation after the one
// dropped here.
void IoContext::finish(detail::IoContextWait& wait,
                       std::error_code error,
                       bool stopped) noexcept {
  wait.disarm();
  {
    const std::lock_guard lock(m_mutex);
    if (wait.cancel_queued) {
      detail::IoContextWait** link = &m_cancels;
      while (*link != &wait) {
        link = &(*link)->next_cancel;
      }
      *link = wait.next_cancel;
      wait.cancel_queued = false;
    }
  }
  wait.complete(error, stopped);
}

}  // namespace makebelieve
