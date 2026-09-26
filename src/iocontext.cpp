// SPDX-License-Identifier: MIT
#include "iocontext.hpp"

#include "iocontextpoller.hpp"
#include "tracer.hpp"

#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace makebelieve {

using detail::Task;
using detail::Wait;

class IoContext::Impl {
  IoContextPoller m_poller;

  // Guards the queues below, which any thread may add to.
  std::mutex m_mutex;
  Task* m_first_task = nullptr;
  Task* m_last_task = nullptr;
  Wait* m_cancels = nullptr;
  bool m_stopping = false;

  // The waits whose handles are with the poller. Touched only by the context's
  // thread.
  std::unordered_set<Wait*> m_registered;

  // Declared last so that everything it uses exists before it starts.
  std::thread m_thread;

 public:
  Impl() : m_thread([this] { run(); }) {}

  ~Impl() {
    {
      const std::lock_guard lock(m_mutex);
      m_stopping = true;
    }
    m_poller.wake();
    m_thread.join();
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  void submit(Task& task) noexcept {
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

  void cancel(Wait& wait) noexcept {
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

  // Arms @a wait and registers its handle. On the context's thread.
  void begin_wait(Wait& wait) noexcept {
    wait.arm();
    std::error_code error;
    try {
      m_registered.insert(&wait);
    } catch (...) {
      error = std::make_error_code(std::errc::not_enough_memory);
    }
    if (!error) {
      error = m_poller.add(wait.handle, &wait);
      if (error) {
        m_registered.erase(&wait);
      }
    }
    if (error) {
      finish(wait, error, false);
    }
  }

 private:
  void run() {
    MB_TRACE_THREAD_NAME("io");
    std::vector<IoContextPoller::Ready> ready;
    while (true) {
      Task* tasks = nullptr;
      Wait* cancels = nullptr;
      bool stopping = false;
      {
        const std::lock_guard lock(m_mutex);
        tasks = std::exchange(m_first_task, nullptr);
        m_last_task = nullptr;
        cancels = std::exchange(m_cancels, nullptr);
        for (Wait* wait = cancels; wait != nullptr; wait = wait->next_cancel) {
          wait->cancel_queued = false;
        }
        stopping = m_stopping;
      }

      // Read each link before running the task, which may complete and free
      // it.
      while (tasks != nullptr) {
        Task* const task = std::exchange(tasks, tasks->next);
        task->execute();
      }

      // A cancellation queued by a stop callback that fires while its wait is
      // armed above waits for the next pass; the poller has been woken for it.
      while (cancels != nullptr) {
        Wait* const wait = std::exchange(cancels, cancels->next_cancel);
        if (m_registered.erase(wait) != 0) {
          m_poller.remove(wait->handle, wait);
          finish(*wait, {}, true);
        }
      }

      if (stopping) {
        // Nothing should be left waiting; stop whatever is rather than hang.
        for (Wait* const wait : std::exchange(m_registered, {})) {
          m_poller.remove(wait->handle, wait);
          finish(*wait, {}, true);
        }
        return;
      }

      ready.clear();
      m_poller.wait(ready);
      for (const auto& [key, error] : ready) {
        auto* const wait = static_cast<Wait*>(key);
        // Only a wait still registered: an earlier one in this batch may have
        // completed and freed another.
        if (m_registered.erase(wait) != 0) {
          m_poller.remove(wait->handle, wait);
          finish(*wait, error, false);
        }
      }
    }
  }

  // Resolves @a wait, which is no longer registered. The stop callback goes
  // first, so none can queue a cancellation after the one dropped here.
  void finish(Wait& wait, std::error_code error, bool stopped) noexcept {
    wait.disarm();
    {
      const std::lock_guard lock(m_mutex);
      if (wait.cancel_queued) {
        Wait** link = &m_cancels;
        while (*link != &wait) {
          link = &(*link)->next_cancel;
        }
        *link = wait.next_cancel;
        wait.cancel_queued = false;
      }
    }
    wait.complete(error, stopped);
  }
};

IoContext::IoContext() : m_impl(std::make_unique<Impl>()) {}

IoContext::~IoContext() = default;

void IoContext::submit(detail::Task& task) noexcept {
  m_impl->submit(task);
}

void IoContext::cancel(detail::Wait& wait) noexcept {
  m_impl->cancel(wait);
}

void IoContext::begin_wait(detail::Wait& wait) noexcept {
  m_impl->begin_wait(wait);
}

}  // namespace makebelieve
