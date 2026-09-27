// SPDX-License-Identifier: MIT
#pragma once

namespace makebelieve {

/// @class IntrusiveTask
/// Work queued intrusively - on an `IoContext`, or waiting in a
/// `BuildQueue`. Calls a `noexcept` callable it does not own, without the
/// callable's type inheriting anything or declaring anything virtual. It holds
/// the target and the stateless model that knows its type, so it never
/// allocates, and the link it is queued by, so queueing it cannot fail either.
/// Lives in the object that queues it - typically the target itself - which
/// holds it in place while it is queued.
class IntrusiveTask {
  // The operations, given the target they act on.
  struct Concept {
    virtual void call(void* target) const noexcept = 0;

   protected:
    Concept() = default;
    ~Concept() = default;  // Only the singletons exist, and are never deleted.

   public:
    Concept(const Concept&) = delete;
    Concept& operator=(const Concept&) = delete;
    Concept(Concept&&) = delete;
    Concept& operator=(Concept&&) = delete;
  };

  // Stateless, so one instance per T serves every IntrusiveTask holding a T.
  template <class T>
  struct Model final : Concept {
    static const Model singleton;

    void call(void* target) const noexcept override {
      (*static_cast<T*>(target))();
    }
  };

  void* m_target;
  const Concept* m_concept;

 public:
  /// The next task in whatever queue holds it, guarded by that queue's lock.
  IntrusiveTask* next = nullptr;

  /// Calls @a target when run. A target whose call operator is private can
  /// befriend this class.
  template <class T>
    requires requires(T& target) {
      { target() } noexcept;
    }
  explicit IntrusiveTask(T& target) noexcept
      : m_target(&target), m_concept(&Model<T>::singleton) {}

  IntrusiveTask(const IntrusiveTask&) = delete;
  IntrusiveTask& operator=(const IntrusiveTask&) = delete;
  IntrusiveTask(IntrusiveTask&&) = delete;
  IntrusiveTask& operator=(IntrusiveTask&&) = delete;
  ~IntrusiveTask() = default;

  void operator()() const noexcept { m_concept->call(m_target); }
};

// Built at compile time, so it is ready before any code runs, whichever thread
// first reaches it.
template <class T>
constinit const IntrusiveTask::Model<T> IntrusiveTask::Model<T>::singleton{};

}  // namespace makebelieve
