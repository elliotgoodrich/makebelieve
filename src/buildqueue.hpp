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
/// The count is cooperative accounting, not a limit on the CPU: it limits how
/// many leases hold a slot, not what runs. Work started through
/// `schedule_leased()` may give its slot back through its @link Lease while
/// it waits on something, and take one again, as often as it likes; whatever
/// it has running meanwhile - threads, processes - keeps running. A lease
/// taking its slot back is served ahead of work that
/// has yet to start, so work under way is not starved by work queued behind
/// it.
///
/// Work that had to wait is resumed on @a Handoff, the scheduler given at
/// construction: started there once a slot is handed to it, or completed
/// stopped there if a stop takes it off the queue first. Giving a slot back
/// therefore only passes it on - to the oldest waiter, or to the free slots -
/// and never runs a waiter's work, or what follows it, on the thread that
/// gave it back. That keeps every slot available the moment it is given back,
/// whatever that thread goes on to do, keeps the stack from growing however
/// many waiters complete at once, keeps anything from completing within
/// the stop callback that stopped it, and lets a slot be given back while the
/// thread doing so holds a lock of its own. Satisfying `stdexec::scheduler` is
/// not enough for that: @a Handoff must defer the work it is given, never
/// running it within `start()` as an inline scheduler would.
///
/// @pre Nothing is running or waiting when it is destroyed.
template <stdexec::scheduler Handoff>
class BuildQueue {
  class Slot;
  class SlotSender;
  class ReacquireSender;
  template <class Receiver, bool Reacquire>
  class SlotOperation;

 public:
  class Lease;

 private:
  // One queue of waiters, oldest first, linked through each one's `next`.
  struct Waiters {
    IntrusiveTask* first = nullptr;
    IntrusiveTask* last = nullptr;

    void push(IntrusiveTask& waiter) noexcept {
      waiter.next = nullptr;
      if (last == nullptr) {
        first = &waiter;
      } else {
        last->next = &waiter;
      }
      last = &waiter;
    }

    IntrusiveTask* pop() noexcept {
      IntrusiveTask* const waiter = first;
      if (waiter != nullptr) {
        first = waiter->next;
        if (first == nullptr) {
          last = nullptr;
        }
      }
      return waiter;
    }

    // Removes @a waiter if it is queued here, reporting whether it was.
    bool remove(IntrusiveTask& waiter) noexcept {
      IntrusiveTask* previous = nullptr;
      for (IntrusiveTask* it = first; it != nullptr; it = it->next) {
        if (it == &waiter) {
          (previous == nullptr ? first : previous->next) = it->next;
          if (last == it) {
            last = previous;
          }
          return true;
        }
        previous = it;
      }
      return false;
    }
  };

  // Guards everything below.
  std::mutex m_mutex;
  std::size_t m_free;

  // Leases taking their slot back, served before new work.
  Waiters m_reacquiring;

  // New work waiting for a slot.
  Waiters m_waiting;

  Handoff m_handoff;

  // Takes a slot if one is free. @pre m_mutex is held.
  [[nodiscard]] bool try_take() noexcept {
    if (m_free == 0) {
      return false;
    }
    --m_free;
    return true;
  }

  // The queue a waiter of that kind waits in. @pre m_mutex is held.
  Waiters& waiters(bool reacquire) noexcept {
    return reacquire ? m_reacquiring : m_waiting;
  }

  // Gives a slot back: to the oldest lease taking one back, else to the
  // oldest new work, which then resumes on m_handoff, or to the free slots.
  void release() noexcept {
    IntrusiveTask* waiter = nullptr;
    {
      const std::lock_guard lock(m_mutex);
      waiter = m_reacquiring.pop();
      if (waiter == nullptr) {
        waiter = m_waiting.pop();
      }
      if (waiter == nullptr) {
        ++m_free;
        return;
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

  /// A sender that starts the work @a make returns, given the @link Lease on
  /// its slot, once a slot is free, and completes as that work does - giving
  /// the slot back, if the lease holds it then, as the work completes and
  /// before passing on how it completed, and whenever the operation itself is
  /// destroyed. Stopped before a slot is free, it leaves the queue and
  /// completes stopped without calling @a make.
  /// @pre The work does not complete while a `Lease::reacquire()` it started is
  /// still under way.
  template <class MakeWork>
  [[nodiscard]] auto schedule_leased(MakeWork make) {
    return take() |
           stdexec::let_value([make = std::move(make)](Slot& slot) mutable {
             return stdexec::just(Lease(slot)) |
                    stdexec::let_value([&make](Lease& lease) {
                      // Not left to the Lease's destructor: this operation may
                      // outlive the work by a long way - a when_all's lives
                      // until all its work is done, which may need this very
                      // slot.
                      return exec::finally(
                          make(lease),
                          stdexec::just() | stdexec::then([&lease]() noexcept {
                            lease.release();
                          }));
                    });
           });
  }

  /// A sender that starts @a work once a slot is free and completes as it
  /// does, giving the slot back as @a work completes - before passing on how
  /// it completed, and whenever the operation itself is destroyed. Stopped
  /// before a slot is free, it leaves the queue and completes stopped without
  /// starting @a work.
  template <stdexec::sender Work>
  [[nodiscard]] auto schedule(Work work) {
    return schedule_leased(
        [work = std::move(work)](Lease&) mutable { return std::move(work); });
  }
};

// One slot, given back to its queue by release() or, failing that, when
// destroyed.
template <stdexec::scheduler Handoff>
class BuildQueue<Handoff>::Slot {
  friend class Lease;

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

/// @class BuildQueue::Lease
/// The claim a piece of work started by `schedule_leased()` has on the queue:
/// holding a slot at first, it can give that slot back and take one again, as
/// often as it likes. Only one thread at a time may use it, and nothing
/// @link reacquire starts completes on the thread calling it except within
/// its `start()`.
template <stdexec::scheduler Handoff>
class BuildQueue<Handoff>::Lease {
  template <class Receiver, bool Reacquire>
  friend class SlotOperation;
  friend class ReacquireSender;

  BuildQueue* m_queue;
  bool m_held = true;

 public:
  /// Takes over @a slot.
  explicit Lease(Slot& slot) noexcept
      : m_queue(std::exchange(slot.m_queue, nullptr)) {}

  Lease(Lease&& other) noexcept
      : m_queue(other.m_queue), m_held(std::exchange(other.m_held, false)) {}

  Lease& operator=(Lease&&) = delete;
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

  ~Lease() { release(); }

  /// Whether this lease holds a slot.
  [[nodiscard]] bool held() const noexcept { return m_held; }

  /// Gives the slot back, if held, passing it on exactly as the end of the
  /// work would: nothing waiting for it runs on this thread.
  void release() noexcept {
    if (std::exchange(m_held, false)) {
      m_queue->release();
    }
  }

  /// A sender that completes, holding a slot again, once one is free - within
  /// its `start()` if one already is, otherwise on Handoff once one is handed
  /// to it, ahead of any work that has yet to start. Stopped first, it leaves
  /// the queue and completes stopped on Handoff, holding nothing.
  /// @pre Not @link held, and no other reacquire() under way.
  [[nodiscard]] ReacquireSender reacquire() noexcept {
    return ReacquireSender(*this);
  }
};

// Waits for a slot: either new work taking its first one, completing with a
// Slot, or a Lease taking one back (@a Reacquire), completing with nothing.
template <stdexec::scheduler Handoff>
template <class Receiver, bool Reacquire>
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
        self->complete_given();
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
  Lease* m_lease;  // the lease taking a slot back, if Reacquire
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
      if (!m_queue->waiters(Reacquire).remove(m_waiting)) {
        m_stopped_early = true;  // Not queued yet, or already given a slot.
        return;
      }
    }
    // Off the queue, so this is the only completion. Not from here, which is
    // within the stop callback, but on Handoff.
    m_resume.emplace(ConnectResume{this, false});
    stdexec::start(*m_resume);
  }

  void complete_given() noexcept {
    m_on_stop.reset();
    if constexpr (Reacquire) {
      m_lease->m_held = true;
      stdexec::set_value(std::move(m_receiver));
    } else {
      stdexec::set_value(std::move(m_receiver), Slot(*m_queue));
    }
  }

  void complete_stopped() noexcept {
    m_on_stop.reset();
    stdexec::set_stopped(std::move(m_receiver));
  }

 public:
  using operation_state_concept = stdexec::operation_state_t;

  SlotOperation(BuildQueue& queue, Lease* lease, Receiver receiver)
      : m_queue(&queue), m_lease(lease), m_receiver(std::move(receiver)) {}

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
        m_queue->waiters(Reacquire).push(m_waiting);
      }
    }
    if (stopped) {
      complete_stopped();
    } else if (given) {
      complete_given();
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
  SlotOperation<Receiver, false> connect(Receiver receiver) const {
    return SlotOperation<Receiver, false>(*m_queue, nullptr,
                                          std::move(receiver));
  }
};

template <stdexec::scheduler Handoff>
class BuildQueue<Handoff>::ReacquireSender {
  Lease* m_lease;

 public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(),
                                     stdexec::set_stopped_t()>;

  explicit ReacquireSender(Lease& lease) noexcept : m_lease(&lease) {}

  template <class Receiver>
  SlotOperation<Receiver, true> connect(Receiver receiver) const {
    return SlotOperation<Receiver, true>(*m_lease->m_queue, m_lease,
                                         std::move(receiver));
  }
};

}  // namespace makebelieve
