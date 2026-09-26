// SPDX-License-Identifier: MIT
#pragma once

namespace makebelieve::detail {

/// @class Task
/// Work queued intrusively - on an `IoContext`, or waiting in a
/// `BuildQueue`. Runs `T::execute()` on an object it does not own, without
/// `T` inheriting anything or declaring anything virtual. It holds the target
/// and the stateless model that knows its type, so it never allocates, and the
/// link it is queued by, so queueing it cannot fail either. Lives in the
/// operation state that queues it, which holds it in place while it is queued.
class Task {
  // The operations, given the target they act on.
  struct Concept {
    virtual void execute(void* target) const noexcept = 0;

   protected:
    Concept() = default;
    ~Concept() = default;  // Only the singletons exist, and are never deleted.

   public:
    Concept(const Concept&) = delete;
    Concept& operator=(const Concept&) = delete;
    Concept(Concept&&) = delete;
    Concept& operator=(Concept&&) = delete;
  };

  // Stateless, so one instance per T serves every Task holding a T.
  template <class T>
  struct Model final : Concept {
    static const Model singleton;

    void execute(void* target) const noexcept override {
      static_cast<T*>(target)->execute();
    }
  };

  void* m_target;
  const Concept* m_concept;

 public:
  /// The next task in whatever queue holds it, guarded by that queue's lock.
  Task* next = nullptr;

  template <class T>
    requires requires(T& target) {
      { target.execute() } noexcept;
    }
  explicit Task(T& target) noexcept
      : m_target(&target), m_concept(&Model<T>::singleton) {}

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  Task(Task&&) = delete;
  Task& operator=(Task&&) = delete;
  ~Task() = default;

  void execute() const noexcept { m_concept->execute(m_target); }
};

// Built at compile time, so it is ready before any code runs, whichever thread
// first reaches it.
template <class T>
constinit const Task::Model<T> Task::Model<T>::singleton{};

}  // namespace makebelieve::detail
