// SPDX-License-Identifier: MIT
#pragma once

#include "intrusivetask.hpp"
#include "nativehandle.hpp"
#include "nativehandlepoller.hpp"

#include <exec/repeat_until.hpp>
#include <stdexec/execution.hpp>

#include <concepts>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace makebelieve {

/// @class IoContext
/// Waits on operating-system handles on behalf of senders: a Linux file
/// descriptor becoming readable, or a Windows handle becoming signalled.
///
/// The context owns one thread, which does all of the waiting and completes
/// every sender the context hands out - including a wait that is cancelled, so
/// no sender of ours ever completes from within the stop callback that
/// cancelled it. Work that follows a wait runs on that thread unless it moves
/// elsewhere, so it should be brief.
///
/// @pre Nothing is waiting on the context, or queued on its scheduler, when it
/// is destroyed.
class IoContext {
 public:
  /// A file descriptor on Linux, a `HANDLE` on Windows.
  using NativeHandle = makebelieve::NativeHandle;

  class Scheduler;
  class WaitSender;

  IoContext();
  ~IoContext();

  IoContext(const IoContext&) = delete;
  IoContext& operator=(const IoContext&) = delete;
  IoContext(IoContext&&) = delete;
  IoContext& operator=(IoContext&&) = delete;

  /// A scheduler whose work runs on the context's thread.
  [[nodiscard]] Scheduler get_scheduler() noexcept;

  /// A sender that completes with no value once @a handle is readable (Linux)
  /// or signalled (Windows) - at once if it already is - with the
  /// `error_code` if the context could not wait on it, or stopped if a stop is
  /// requested first. Waiting consumes nothing: the caller reads the
  /// descriptor, or resets a manual-reset event, itself. A Windows
  /// auto-reset event is reset by the wait, as by any other wait on it.
  /// @pre Nothing else waits on @a handle through this context at the same
  /// time, and @a handle stays open until the sender completes.
  [[nodiscard]] WaitSender async_wait(NativeHandle handle) noexcept;

  /// Starts a loop in @a scope that waits on @a handle and calls @a on_ready,
  /// on the context's thread, each time it is ready - until @a scope is
  /// stopped or waiting on @a handle fails. @a on_ready consumes the readiness
  /// (reads the descriptor, resets the event), or it is called again at once.
  /// @pre The owner of @a scope stops and joins it before @a handle is closed,
  /// and not from the context's thread.
  template <std::invocable OnReady>
    requires std::is_nothrow_invocable_v<OnReady&>
  void spawn_watch(NativeHandle handle,
                   stdexec::counting_scope& scope,
                   OnReady on_ready);

 private:
  class Wait;
  template <class Receiver>
  class ScheduleOperation;
  template <class Receiver>
  class WaitOperation;

  NativeHandlePoller m_poller;

  // Guards the queues below, which any thread may add to.
  std::mutex m_mutex;
  IntrusiveTask* m_first_task = nullptr;
  IntrusiveTask* m_last_task = nullptr;
  Wait* m_cancels = nullptr;
  bool m_stopping = false;

  // The waits whose handles are with the poller. Touched only by the context's
  // thread.
  std::unordered_set<Wait*> m_registered;

  // Declared last so that everything it uses exists before it starts.
  std::jthread m_thread;

  // Thread-safe: queue @a task to run on the context's thread.
  void submit(IntrusiveTask& task) noexcept;

  // Thread-safe: ask the context's thread to stop @a wait.
  void cancel(Wait& wait) noexcept;

  // On the context's thread: arm @a wait and register its handle.
  void begin_wait(Wait& wait) noexcept;

  // The context's thread.
  void run();

  // On the context's thread: resolves @a wait, which is no longer registered.
  void finish(Wait& wait, std::error_code error, bool stopped) noexcept;
};

// A wait on one handle, as an `IoContext` keeps it while it is registered:
// the handle, the links the context lists it by, and the part that depends on
// who is waiting - installing and removing the stop callback, and completing
// the sender - erased from a `T` it does not own in the same way as
// `IntrusiveTask`. Lives in the operation state that waits.
class IoContext::Wait {
  struct Concept {
    virtual void arm(void* target) const noexcept = 0;
    virtual void disarm(void* target) const noexcept = 0;
    virtual void complete(void* target,
                          std::error_code error,
                          bool stopped) const noexcept = 0;

   protected:
    Concept() = default;
    ~Concept() = default;  // Only the singletons exist, and are never deleted.

   public:
    Concept(const Concept&) = delete;
    Concept& operator=(const Concept&) = delete;
    Concept(Concept&&) = delete;
    Concept& operator=(Concept&&) = delete;
  };

  template <class T>
  struct Model final : Concept {
    static const Model singleton;

    void arm(void* target) const noexcept override {
      static_cast<T*>(target)->arm();
    }

    void disarm(void* target) const noexcept override {
      static_cast<T*>(target)->disarm();
    }

    void complete(void* target,
                  std::error_code error,
                  bool stopped) const noexcept override {
      static_cast<T*>(target)->complete(error, stopped);
    }
  };

  void* m_target;
  const Concept* m_concept;

 public:
  /// What is waited on.
  NativeHandle handle;

  /// Links the wait into the context's list of cancellations, guarded by the
  /// context's lock.
  Wait* next_cancel = nullptr;
  bool cancel_queued = false;

  template <class T>
    requires requires(T& target, std::error_code error, bool stopped) {
      { target.arm() } noexcept;
      { target.disarm() } noexcept;
      { target.complete(error, stopped) } noexcept;
    }
  Wait(T& target, NativeHandle handle) noexcept
      : m_target(&target), m_concept(&Model<T>::singleton), handle(handle) {}

  Wait(const Wait&) = delete;
  Wait& operator=(const Wait&) = delete;
  Wait(Wait&&) = delete;
  Wait& operator=(Wait&&) = delete;
  ~Wait() = default;

  /// Installs the stop callback. May run the callback inline.
  void arm() const noexcept { m_concept->arm(m_target); }

  /// Removes the stop callback, waiting out one running on another thread.
  void disarm() const noexcept { m_concept->disarm(m_target); }

  /// Completes the sender: stopped if @a stopped, otherwise with @a error if
  /// set, otherwise with no value.
  void complete(std::error_code error, bool stopped) const noexcept {
    m_concept->complete(m_target, error, stopped);
  }
};

template <class T>
constinit const IoContext::Wait::Model<T>
    IoContext::Wait::Model<T>::singleton{};

template <class Receiver>
class IoContext::ScheduleOperation {
  friend class IntrusiveTask;

  IoContext* m_context;
  Receiver m_receiver;
  IntrusiveTask m_task{*this};

  // Run on the context's thread once queued.
  void operator()() noexcept {
    if (stdexec::get_stop_token(stdexec::get_env(m_receiver))
            .stop_requested()) {
      stdexec::set_stopped(std::move(m_receiver));
    } else {
      stdexec::set_value(std::move(m_receiver));
    }
  }

 public:
  using operation_state_concept = stdexec::operation_state_t;

  ScheduleOperation(IoContext& context, Receiver receiver)
      : m_context(&context), m_receiver(std::move(receiver)) {}

  ScheduleOperation(const ScheduleOperation&) = delete;
  ScheduleOperation& operator=(const ScheduleOperation&) = delete;
  ScheduleOperation(ScheduleOperation&&) = delete;
  ScheduleOperation& operator=(ScheduleOperation&&) = delete;
  ~ScheduleOperation() = default;

  void start() & noexcept { m_context->submit(m_task); }
};

// Started by queueing a task that, on the context's thread, arms the stop
// callback and registers the handle; the context then resolves the wait
// exactly once, by disarming and completing it.
template <class Receiver>
class IoContext::WaitOperation {
  friend class IntrusiveTask;
  friend class Wait;

  struct OnStop {
    WaitOperation* self;
    void operator()() const noexcept { self->m_context->cancel(self->m_wait); }
  };
  using Token = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

  IoContext* m_context;
  Receiver m_receiver;
  std::optional<stdexec::stop_callback_for_t<Token, OnStop>> m_on_stop;
  IntrusiveTask m_start{*this};
  Wait m_wait;

  // Run on the context's thread once queued: arms and registers the wait.
  void operator()() noexcept { m_context->begin_wait(m_wait); }

  void arm() noexcept {
    m_on_stop.emplace(stdexec::get_stop_token(stdexec::get_env(m_receiver)),
                      OnStop{this});
  }

  void disarm() noexcept { m_on_stop.reset(); }

  void complete(std::error_code error, bool stopped) noexcept {
    if (stopped) {
      stdexec::set_stopped(std::move(m_receiver));
    } else if (error) {
      stdexec::set_error(std::move(m_receiver), error);
    } else {
      stdexec::set_value(std::move(m_receiver));
    }
  }

 public:
  using operation_state_concept = stdexec::operation_state_t;

  WaitOperation(IoContext& context, NativeHandle handle, Receiver receiver)
      : m_context(&context),
        m_receiver(std::move(receiver)),
        m_wait(*this, handle) {}

  WaitOperation(const WaitOperation&) = delete;
  WaitOperation& operator=(const WaitOperation&) = delete;
  WaitOperation(WaitOperation&&) = delete;
  WaitOperation& operator=(WaitOperation&&) = delete;
  ~WaitOperation() = default;

  void start() & noexcept { m_context->submit(m_start); }
};

class IoContext::Scheduler {
  IoContext* m_context;

 public:
  class Sender {
    IoContext* m_context;

    struct Env {
      IoContext* context;
      [[nodiscard]] Scheduler query(
          stdexec::get_completion_scheduler_t<stdexec::set_value_t> /*tag*/)
          const noexcept {
        return Scheduler(*context);
      }
    };

   public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(),
                                       stdexec::set_stopped_t()>;

    explicit Sender(IoContext& context) noexcept : m_context(&context) {}

    template <class Receiver>
    ScheduleOperation<Receiver> connect(Receiver receiver) const {
      return ScheduleOperation<Receiver>(*m_context, std::move(receiver));
    }

    [[nodiscard]] Env get_env() const noexcept { return Env{m_context}; }
  };

  using scheduler_concept = stdexec::scheduler_t;

  explicit Scheduler(IoContext& context) noexcept : m_context(&context) {}

  [[nodiscard]] Sender schedule() const noexcept { return Sender(*m_context); }

  [[nodiscard]] friend bool operator==(const Scheduler&,
                                       const Scheduler&) = default;
};

class IoContext::WaitSender {
  IoContext* m_context;
  NativeHandle m_handle;

 public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(),
                                     stdexec::set_error_t(std::error_code),
                                     stdexec::set_stopped_t()>;

  WaitSender(IoContext& context, NativeHandle handle) noexcept
      : m_context(&context), m_handle(handle) {}

  template <class Receiver>
  WaitOperation<Receiver> connect(Receiver receiver) const {
    return WaitOperation<Receiver>(*m_context, m_handle, std::move(receiver));
  }
};

inline IoContext::Scheduler IoContext::get_scheduler() noexcept {
  return Scheduler(*this);
}

inline IoContext::WaitSender IoContext::async_wait(
    NativeHandle handle) noexcept {
  return {*this, handle};
}

template <std::invocable OnReady>
  requires std::is_nothrow_invocable_v<OnReady&>
void IoContext::spawn_watch(NativeHandle handle,
                            stdexec::counting_scope& scope,
                            OnReady on_ready) {
  stdexec::spawn(
      async_wait(handle) |
          stdexec::then([on_ready = std::move(on_ready)]() mutable noexcept {
            on_ready();
            return false;
          }) |
          exec::repeat_until() |
          stdexec::upon_error([](const auto& /*error*/) noexcept {}),
      scope.get_token());
}

}  // namespace makebelieve
