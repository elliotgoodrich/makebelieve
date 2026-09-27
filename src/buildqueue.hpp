// SPDX-License-Identifier: MIT
#pragma once

#include "intrusivetask.hpp"

#include <exec/finally.hpp>
#include <stdexec/execution.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace makebelieve {

/// @class BuildQueue
/// Runs work at most a fixed number of senders at a time. `schedule()`
/// starts the work once fewer than that are running and holds its slot until
/// it completes; the rest wait their turn in the order they were scheduled,
/// from one queue, so that a later ordering - by priority, say - has one place
/// to change. It knows nothing about what the work is.
///
/// Work that had to wait is resumed on @a Handoff, the scheduler given at
/// construction: started there once a slot is handed to it, or completed
/// stopped there if a stop takes it off the queue first. Giving a slot back
/// therefore only passes it on - to the oldest waiter, or to the free slots -
/// and never runs a waiter's work, or what follows it, on the thread that
/// gave it back. That keeps every slot available the moment it is given back,
/// whatever that thread goes on to do, keeps the stack from growing however
/// many waiters complete at once, and keeps anything from completing within
/// the stop callback that stopped it. Satisfying `stdexec::scheduler` is not
/// enough for that: @a Handoff must defer the work it is given, never running
/// it within `start()` as an inline scheduler would.
///
/// @pre Nothing is running or waiting when it is destroyed.
template <stdexec::scheduler Handoff>
class BuildQueue {
  class Slot;
  class SlotSender;
  template <class Receiver>
  class SlotOperation;

  // Guards everything below.
  std::mutex m_mutex;
  std::size_t m_free;

  // What is waiting for a slot, oldest first, linked through each one's
  // `next`.
  IntrusiveTask* m_first = nullptr;
  IntrusiveTask* m_last = nullptr;

  Handoff m_handoff;

  // Takes a slot if one is free. @pre m_mutex is held.
  [[nodiscard]] bool try_take() noexcept {
    if (m_free == 0) {
      return false;
    }
    --m_free;
    return true;
  }

  // Queues @a waiter. @pre m_mutex is held.
  void enqueue(IntrusiveTask& waiter) noexcept {
    waiter.next = nullptr;
    if (m_last == nullptr) {
      m_first = &waiter;
    } else {
      m_last->next = &waiter;
    }
    m_last = &waiter;
  }

  // Removes @a waiter if it is still queued, reporting whether it was.
  // @pre m_mutex is held.
  bool dequeue(IntrusiveTask& waiter) noexcept {
    IntrusiveTask* previous = nullptr;
    for (IntrusiveTask* it = m_first; it != nullptr; it = it->next) {
      if (it == &waiter) {
        (previous == nullptr ? m_first : previous->next) = it->next;
        if (m_last == it) {
          m_last = previous;
        }
        return true;
      }
      previous = it;
    }
    return false;
  }

  // Gives a slot back: to the oldest waiter, which then resumes on
  // m_handoff, or to the free slots.
  void release() noexcept {
    IntrusiveTask* waiter = nullptr;
    {
      const std::lock_guard lock(m_mutex);
      waiter = m_first;
      if (waiter == nullptr) {
        ++m_free;
        return;
      }
      m_first = waiter->next;
      if (m_first == nullptr) {
        m_last = nullptr;
      }
    }
    (*waiter)();
  }

  // A sender that completes with a Slot once one is free - at once if one
  // already is - or stopped if a stop is requested first.
  [[nodiscard]] SlotSender take() noexcept { return SlotSender(*this); }

 public:
  /// Creates a queue running at most @a concurrency pieces of work at a time,
  /// resuming work that had to wait on @a handoff.
  /// @pre Whatever runs @a handoff's work outlives this object.
  BuildQueue(std::size_t concurrency, Handoff handoff) noexcept
      : m_free(concurrency), m_handoff(std::move(handoff)) {}

  BuildQueue(const BuildQueue&) = delete;
  BuildQueue& operator=(const BuildQueue&) = delete;
  BuildQueue(BuildQueue&&) = delete;
  BuildQueue& operator=(BuildQueue&&) = delete;
  ~BuildQueue() = default;

  /// A sender that starts @a work once a slot is free and completes as it
  /// does, giving the slot back as @a work completes - before passing on how
  /// it completed, and whenever the operation itself is destroyed. Stopped
  /// before a slot is free, it leaves the queue and completes stopped without
  /// starting @a work.
  template <stdexec::sender Work>
  [[nodiscard]] auto schedule(Work work) {
    return take() |
           stdexec::let_value([work = std::move(work)](Slot& slot) mutable {
             // Not left to the Slot's destructor: this operation may outlive
             // the work by a long way - a when_all's lives until all its work
             // is done, which may need this very slot.
             return exec::finally(
                 std::move(work),
                 stdexec::just() |
                     stdexec::then([&slot]() noexcept { slot.release(); }));
           });
  }
};

// One slot, given back to its queue by release() or, failing that, when
// destroyed.
template <stdexec::scheduler Handoff>
class BuildQueue<Handoff>::Slot {
  BuildQueue* m_queue;

 public:
  explicit Slot(BuildQueue& queue) noexcept : m_queue(&queue) {}

  Slot(Slot&& other) noexcept
      : m_queue(std::exchange(other.m_queue, nullptr)) {}

  Slot& operator=(Slot other) noexcept {
    std::swap(m_queue, other.m_queue);
    return *this;
  }

  Slot(const Slot&) = delete;

  ~Slot() { release(); }

  // Gives the slot back, if it has not been already.
  void release() noexcept {
    if (BuildQueue* const queue = std::exchange(m_queue, nullptr)) {
      queue->release();
    }
  }
};

template <stdexec::scheduler Handoff>
template <class Receiver>
class BuildQueue<Handoff>::SlotOperation {
  friend class IntrusiveTask;

  struct OnStop {
    SlotOperation* self;
    void operator()() const noexcept { self->on_stop(); }
  };
  using Token = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

  // Resumes the waiter on Handoff: given its slot if @a given, otherwise
  // stopped.
  struct Resume {
    using receiver_concept = stdexec::receiver_t;
    SlotOperation* self;
    bool given;

    void set_value() noexcept {
      if (given) {
        self->m_on_stop.reset();
        stdexec::set_value(std::move(self->m_receiver), Slot(*self->m_queue));
      } else {
        self->complete_stopped();
      }
    }

    // Handoff shutting down: a slot this was given goes straight back.
    void set_stopped() noexcept {
      if (given) {
        self->m_queue->release();
      }
      self->complete_stopped();
    }
  };
  using ResumeOperation =
      stdexec::connect_result_t<stdexec::schedule_result_t<Handoff&>, Resume>;

  // Lets std::optional build a ResumeOperation, which cannot be moved, from
  // what connect() returns.
  struct ConnectResume {
    SlotOperation* self;
    bool given;
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    operator ResumeOperation() const {
      return stdexec::connect(stdexec::schedule(self->m_queue->m_handoff),
                              Resume{self, given});
    }
  };

  BuildQueue* m_queue;
  Receiver m_receiver;
  IntrusiveTask m_waiting{*this};
  std::optional<stdexec::stop_callback_for_t<Token, OnStop>> m_on_stop;
  std::optional<ResumeOperation> m_resume;

  // Set, under the queue's lock, when a stop arrives before start() has queued
  // this waiter; start() then completes it stopped itself.
  bool m_stopped_early = false;

  // Given a slot: called by release() once this waiter is off the queue.
  // Resumes on Handoff, so the thread giving the slot back runs none of the
  // work.
  void operator()() noexcept {
    m_resume.emplace(ConnectResume{this, true});
    stdexec::start(*m_resume);
  }

  void on_stop() noexcept {
    {
      const std::lock_guard lock(m_queue->m_mutex);
      if (!m_queue->dequeue(m_waiting)) {
        m_stopped_early = true;  // Not queued yet, or already given a slot.
        return;
      }
    }
    // Off the queue, so this is the only completion. Not from here, which is
    // within the stop callback, but on Handoff.
    m_resume.emplace(ConnectResume{this, false});
    stdexec::start(*m_resume);
  }

  void complete_stopped() noexcept {
    m_on_stop.reset();
    stdexec::set_stopped(std::move(m_receiver));
  }

 public:
  using operation_state_concept = stdexec::operation_state_t;

  SlotOperation(BuildQueue& queue, Receiver receiver)
      : m_queue(&queue), m_receiver(std::move(receiver)) {}

  SlotOperation(const SlotOperation&) = delete;
  SlotOperation& operator=(const SlotOperation&) = delete;
  SlotOperation(SlotOperation&&) = delete;
  SlotOperation& operator=(SlotOperation&&) = delete;
  ~SlotOperation() = default;

  void start() & noexcept {
    // Registered before this waiter can be seen, so a stop that runs the
    // callback inline, right here, finds nothing to take off the queue and
    // leaves completing to the block below.
    m_on_stop.emplace(stdexec::get_stop_token(stdexec::get_env(m_receiver)),
                      OnStop{this});
    bool stopped = false;
    bool given = false;
    {
      const std::lock_guard lock(m_queue->m_mutex);
      if (m_stopped_early) {
        stopped = true;
      } else if (m_queue->try_take()) {
        given = true;
      } else {
        // Once queued, a release on another thread may give this a slot and
        // destroy it at any moment, so nothing below touches it.
        m_queue->enqueue(m_waiting);
      }
    }
    if (stopped) {
      m_on_stop.reset();
      stdexec::set_stopped(std::move(m_receiver));
    } else if (given) {
      m_on_stop.reset();
      stdexec::set_value(std::move(m_receiver), Slot(*m_queue));
    }
  }
};

template <stdexec::scheduler Handoff>
class BuildQueue<Handoff>::SlotSender {
  BuildQueue* m_queue;

 public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(Slot),
                                     stdexec::set_stopped_t()>;

  explicit SlotSender(BuildQueue& queue) noexcept : m_queue(&queue) {}

  template <class Receiver>
  SlotOperation<Receiver> connect(Receiver receiver) const {
    return SlotOperation<Receiver>(*m_queue, std::move(receiver));
  }
};

}  // namespace makebelieve
