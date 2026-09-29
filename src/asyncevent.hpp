// SPDX-License-Identifier: MIT
#pragma once

#include <stdexec/execution.hpp>

#include <mutex>
#include <utility>

namespace makebelieve {
/// @class AsyncEvent
/// A single-consumer, one-shot signal whose sender completes with no value.
/// Completion runs on the thread calling @link set, or inline when a wait
/// starts after the signal. Use `stdexec::continues_on` when continuations
/// must run elsewhere, particularly when signalling from a stop callback.
/// The event does not observe stop tokens or unregister an abandoned wait.
/// @pre At most one operation waits at a time. The event and a pending
/// operation must remain alive until signalling has finished.
class AsyncEvent {
  std::mutex m_mutex;
  bool m_set = false;
  void* m_waiter = nullptr;
  void (*m_complete)(void*) noexcept = nullptr;

 public:
  /// Signals the event permanently and completes a pending wait, if any.
  /// Repeated calls do not complete the same operation again.
  void set() noexcept {
    void* waiter;
    void (*complete)(void*) noexcept;
    {
      const std::lock_guard lock(m_mutex);
      m_set = true;
      waiter = std::exchange(m_waiter, nullptr);
      complete = m_complete;
    }
    if (waiter) {
      complete(waiter);
    }
  }
  /// A connected wait, borrowing its event and owning its receiver.
  template <class Receiver>
  struct Operation {
    /// Identifies this as a stdexec operation state.
    using operation_state_concept = stdexec::operation_state_t;
    /// The event, which must outlive this operation's completion.
    AsyncEvent* event;
    /// Receives exactly one value completion once the event is signalled.
    Receiver receiver;
    /// Completes inline if already signalled, otherwise registers this wait.
    /// @pre Called once, with no other pending wait on the event.
    void start() & noexcept {
      std::unique_lock lock(event->m_mutex);
      if (event->m_set) {
        lock.unlock();
        stdexec::set_value(std::move(receiver));
      } else {
        event->m_waiter = this;
        event->m_complete = [](void* ptr) noexcept {
          auto* self = static_cast<Operation*>(ptr);
          stdexec::set_value(std::move(self->receiver));
        };
      }
    }
  };
  /// A sender borrowing an event; connecting it does not start the wait.
  struct Sender {
    /// Identifies this as a stdexec sender.
    using sender_concept = stdexec::sender_t;
    /// Waits complete with no value and report neither errors nor stopped.
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;
    /// The event to wait on, which must outlive the connected operation.
    AsyncEvent* event;
    /// Connects @a receiver to a wait without starting it.
    template <class Receiver>
    Operation<Receiver> connect(Receiver receiver) const {
      return {event, std::move(receiver)};
    }
  };
  /// Returns a sender that completes when this event is signalled.
  /// @pre The event outlives the connected operation and its completion.
  Sender wait() noexcept { return {this}; }
};
}  // namespace makebelieve
