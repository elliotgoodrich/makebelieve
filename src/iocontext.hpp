// SPDX-License-Identifier: MIT
#pragma once

#include "linkednode.hpp"
#include "nativehandle.hpp"
#include "nativehandlepoller.hpp"

#include <stdexec/execution.hpp>

#include <atomic>
#include <concepts>
#include <functional>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace makebelieve {

class IoContext;

namespace detail {

// Functions that run on the context's thread will be passed an instance of
// `IoThreadPasskey` as proof that they are running there.  This will be
// used to guarantee that these functions are never called from another
// thread.
class IoThreadPasskey;

// Work queued to run on an `IoContext`'s thread. An operation state derives
// from it, so the queue needs no allocation and queueing cannot fail.
class IoContextTask {
 public:
#ifdef NDEBUG
  // The next task in the context's queue. Written before the task is
  // published to the queue, and read only by the context's thread.
  IoContextTask* next = nullptr;
#else
  // What `next` holds while the task is not queued, so that `submit` can
  // catch a task queued twice - in Debug builds only, as the check costs a
  // store per task.
  static IoContextTask* not_queued() noexcept;

  // The next task in the context's queue, or `not_queued()`. Written before
  // the task is published to the queue, and read only by the context's
  // thread.
  IoContextTask* next = not_queued();
#endif

  // Runs the task on the context's thread. May destroy it.
  virtual void execute(const IoThreadPasskey& passkey) noexcept = 0;

  IoContextTask(const IoContextTask&) = delete;
  IoContextTask& operator=(const IoContextTask&) = delete;
  IoContextTask(IoContextTask&&) = delete;
  IoContextTask& operator=(IoContextTask&&) = delete;

 protected:
  IoContextTask() = default;
  ~IoContextTask() = default;  // Never deleted through this base.
};

// A wait on one handle, as an `IoContext` keeps it while it is registered:
// the handle, the links the context lists it by, and the part that depends on
// who is waiting - installing and removing the stop callback, handling the
// handle being ready, and completing the sender - which the operation state
// that derives from it supplies.
class IoContextWait : public LinkedNode<IoContextWait> {
 public:
  // What is waited on.
  NativeHandle handle;

  // Identifies the handle's watch with the poller while it is registered.
  NativeHandlePoller::Token registration{};

  // The next wait in the context's stack of cancellations. Written by the
  // stop callback before it pushes the wait, and read only by the context's
  // thread. The `LinkedNode` links are touched only by the context's thread.
  IoContextWait* next_cancel = nullptr;

  // Installs the stop callback. May run the callback inline.
  virtual void arm(const IoThreadPasskey& passkey) noexcept = 0;

  // Removes the stop callback, waiting out one running on another thread.
  virtual void disarm(const IoThreadPasskey& passkey) noexcept = 0;

  // Runs on the context's thread each time the handle is ready. Returns
  // whether the wait is done, to be unregistered and completed with no value;
  // otherwise it stays registered, and is reported again the next time the
  // handle is ready. Must not complete or free any wait.
  virtual bool handle_ready(const IoThreadPasskey& passkey) noexcept = 0;

  // Completes the sender: stopped if @a stopped, otherwise with @a error if
  // set, otherwise with no value.
  virtual void complete(const IoThreadPasskey& passkey,
                        std::error_code error,
                        bool stopped) noexcept = 0;

  IoContextWait(const IoContextWait&) = delete;
  IoContextWait& operator=(const IoContextWait&) = delete;
  IoContextWait(IoContextWait&&) = delete;
  IoContextWait& operator=(IoContextWait&&) = delete;

 protected:
  explicit IoContextWait(NativeHandle handle) noexcept : handle(handle) {}
  ~IoContextWait() = default;  // Never deleted through this base.
};

// How `IoContext::Scheduler::Sender` completes.
using IoContextScheduleSignatures =
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_stopped_t()>;

// How `IoContext::WaitSender` completes.
using IoContextWaitSignatures =
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_error_t(std::error_code),
                                   stdexec::set_stopped_t()>;

// How the sender `IoContext::watch` returns completes.
using IoContextWatchSignatures =
    stdexec::completion_signatures<stdexec::set_value_t(),
                                   stdexec::set_stopped_t()>;

template <stdexec::receiver_of<detail::IoContextScheduleSignatures> Receiver>
class IoContextScheduleOperation;

// What `IoContext::watch` calls each time the handle is ready: a callable it
// can keep, and call as an lvalue without throwing.
template <class OnReady>
concept IoContextOnReady =
    std::move_constructible<OnReady> && std::is_nothrow_invocable_v<OnReady&>;

template <class Signatures, stdexec::receiver_of<Signatures> Receiver>
class IoContextWaitOperationBase;

}  // namespace detail

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
  template <stdexec::receiver_of<detail::IoContextScheduleSignatures> Receiver>
  friend class detail::IoContextScheduleOperation;
  template <class Signatures, stdexec::receiver_of<Signatures> Receiver>
  friend class detail::IoContextWaitOperationBase;

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

  /// A sender that waits on @a handle and calls @a on_ready, on the context's
  /// thread, each time it is ready - completing stopped once a stop is
  /// requested, or with no value once waiting on @a handle fails. @a on_ready
  /// consumes the readiness (reads the descriptor, resets the event), or it is
  /// called again at once. The handle stays registered with the context from
  /// start to completion, rather than once for each time it is ready. Spawned
  /// into a `counting_scope`, it is stopped by stopping the scope.
  /// @pre Nothing else waits on @a handle through this context at the same
  /// time, and @a handle stays open until the sender completes.
  template <detail::IoContextOnReady OnReady>
  [[nodiscard]] stdexec::sender_of<stdexec::set_value_t(), stdexec::env<>> auto
  watch(NativeHandle handle, OnReady on_ready);

  /// Spawns `watch(handle, on_ready)` into @a scope, so that stopping the
  /// scope ends it. Defined out of line, so its callers instantiate none of
  /// the sender.
  /// @pre As for `watch`, and @a on_ready is not empty. The owner of @a scope
  /// stops and joins it before @a handle is closed, and not from the
  /// context's thread.
  void spawn_watch(NativeHandle handle,
                   stdexec::counting_scope& scope,
                   std::move_only_function<void() noexcept> on_ready);

 private:
  NativeHandlePoller m_poller;

  // The tasks queued to run, newest first, which any thread may push to.
  std::atomic<detail::IoContextTask*> m_tasks = nullptr;

  // The waits whose stop callbacks have fired, newest first, which any thread
  // may push to.
  std::atomic<detail::IoContextWait*> m_cancels = nullptr;

  std::atomic<bool> m_stopping = false;

  // The heads of the lists of waits that have their handles with the poller:
  // those not cancelled, and those taken from `m_cancels` to be stopped. A
  // wait being finished may briefly be in `m_cancelled` while unregistered.
  // Touched only by the context's thread.
  LinkedNode<detail::IoContextWait> m_registered;
  LinkedNode<detail::IoContextWait> m_cancelled;

  // Declared last so that everything it uses exists before it starts.
  std::jthread m_thread;

  // Thread-safe: queue @a task to run on the context's thread.
  void submit(detail::IoContextTask& task) noexcept;

  // Thread-safe: ask the context's thread to stop @a wait.
  void cancel(detail::IoContextWait& wait) noexcept;

  // On the context's thread: arm @a wait and register its handle.
  void begin_wait(const detail::IoThreadPasskey& passkey,
                  detail::IoContextWait& wait) noexcept;

  // The context's thread.
  void run(const detail::IoThreadPasskey& passkey) noexcept;

  // On the context's thread: move each wait on `m_cancels` into
  // `m_cancelled`.
  void take_cancels(const detail::IoThreadPasskey& passkey) noexcept;

  // On the context's thread: take @a wait's handle back from the poller.
  void unregister(const detail::IoThreadPasskey& passkey,
                  detail::IoContextWait& wait) noexcept;

  // On the context's thread: resolves @a wait, which is no longer registered,
  // taking it out of whichever list holds it.
  void finish(const detail::IoThreadPasskey& passkey,
              detail::IoContextWait& wait,
              std::error_code error,
              bool stopped) noexcept;
};

namespace detail {

template <stdexec::receiver_of<detail::IoContextScheduleSignatures> Receiver>
class IoContextScheduleOperation final : IoContextTask {
  IoContext* m_context;
  Receiver m_receiver;

  // Run on the context's thread once queued.
  void execute(const IoThreadPasskey& /*passkey*/) noexcept override {
    if (stdexec::get_stop_token(stdexec::get_env(m_receiver))
            .stop_requested()) {
      stdexec::set_stopped(std::move(m_receiver));
    } else {
      stdexec::set_value(std::move(m_receiver));
    }
  }

 public:
  using operation_state_concept = stdexec::operation_state_t;

  IoContextScheduleOperation(IoContext& context, Receiver receiver)
      : m_context(&context), m_receiver(std::move(receiver)) {}

  IoContextScheduleOperation(const IoContextScheduleOperation&) = delete;
  IoContextScheduleOperation& operator=(const IoContextScheduleOperation&) =
      delete;
  IoContextScheduleOperation(IoContextScheduleOperation&&) = delete;
  IoContextScheduleOperation& operator=(IoContextScheduleOperation&&) = delete;
  ~IoContextScheduleOperation() = default;

  void start() & noexcept { m_context->submit(*this); }
};

// The part of a wait's operation state that `async_wait` and `watch` share,
// for a receiver of the @a Signatures the wait completes with. Started by
// queueing itself as a task that, on the context's thread, completes stopped
// if a stop is already requested, and otherwise arms the stop callback and
// registers the handle; the context then resolves the wait exactly once, by
// disarming and completing it.
template <class Signatures, stdexec::receiver_of<Signatures> Receiver>
class IoContextWaitOperationBase : IoContextTask, protected IoContextWait {
  struct OnStop {
    IoContextWaitOperationBase* self;
    void operator()() const noexcept { self->m_context->cancel(*self); }
  };
  using Token = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

  std::optional<stdexec::stop_callback_for_t<Token, OnStop>> m_on_stop;

  // Run on the context's thread once queued. A wait already stopped is never
  // armed or registered, so it completes here with nothing to undo; a stop
  // requested after this check is handled by the stop callback.
  void execute(const IoThreadPasskey& passkey) noexcept override {
    if (stdexec::get_stop_token(stdexec::get_env(m_receiver))
            .stop_requested()) {
      stdexec::set_stopped(std::move(m_receiver));
    } else {
      m_context->begin_wait(passkey, *this);
    }
  }

  void arm(const IoThreadPasskey& /*passkey*/) noexcept override {
    m_on_stop.emplace(stdexec::get_stop_token(stdexec::get_env(m_receiver)),
                      OnStop{this});
  }

  void disarm(const IoThreadPasskey& /*passkey*/) noexcept override {
    m_on_stop.reset();
  }

 protected:
  IoContext* m_context;
  Receiver m_receiver;

  IoContextWaitOperationBase(IoContext& context,
                             NativeHandle handle,
                             Receiver receiver)
      : IoContextWait(handle),
        m_context(&context),
        m_receiver(std::move(receiver)) {}

  ~IoContextWaitOperationBase() = default;

 public:
  using operation_state_concept = stdexec::operation_state_t;

  IoContextWaitOperationBase(const IoContextWaitOperationBase&) = delete;
  IoContextWaitOperationBase& operator=(const IoContextWaitOperationBase&) =
      delete;
  IoContextWaitOperationBase(IoContextWaitOperationBase&&) = delete;
  IoContextWaitOperationBase& operator=(IoContextWaitOperationBase&&) = delete;

  void start() & noexcept { m_context->submit(*this); }
};

// `IoContext::async_wait`: done the first time the handle is ready.
template <stdexec::receiver_of<detail::IoContextWaitSignatures> Receiver>
class IoContextWaitOperation final
    : public IoContextWaitOperationBase<IoContextWaitSignatures, Receiver> {
  bool handle_ready(const IoThreadPasskey& /*passkey*/) noexcept override {
    return true;
  }

  void complete(const IoThreadPasskey& /*passkey*/,
                std::error_code error,
                bool stopped) noexcept override {
    if (stopped) {
      stdexec::set_stopped(std::move(this->m_receiver));
    } else if (error) {
      stdexec::set_error(std::move(this->m_receiver), error);
    } else {
      stdexec::set_value(std::move(this->m_receiver));
    }
  }

 public:
  IoContextWaitOperation(IoContext& context,
                         NativeHandle handle,
                         Receiver receiver)
      : IoContextWaitOperationBase<IoContextWaitSignatures, Receiver>(
            context,
            handle,
            std::move(receiver)) {}

  IoContextWaitOperation(const IoContextWaitOperation&) = delete;
  IoContextWaitOperation& operator=(const IoContextWaitOperation&) = delete;
  IoContextWaitOperation(IoContextWaitOperation&&) = delete;
  IoContextWaitOperation& operator=(IoContextWaitOperation&&) = delete;
  ~IoContextWaitOperation() = default;
};

// `IoContext::watch`: calls @a OnReady each time the handle is ready, and so
// stays registered until it is stopped. Completes with no value if the handle
// cannot be registered, dropping the error.
template <stdexec::receiver_of<detail::IoContextWatchSignatures> Receiver,
          IoContextOnReady OnReady>
class IoContextWatchOperation final
    : public IoContextWaitOperationBase<IoContextWatchSignatures, Receiver> {
  OnReady m_on_ready;

  bool handle_ready(const IoThreadPasskey& /*passkey*/) noexcept override {
    m_on_ready();
    return false;
  }

  void complete(const IoThreadPasskey& /*passkey*/,
                std::error_code /*error*/,
                bool stopped) noexcept override {
    if (stopped) {
      stdexec::set_stopped(std::move(this->m_receiver));
    } else {
      stdexec::set_value(std::move(this->m_receiver));
    }
  }

 public:
  IoContextWatchOperation(IoContext& context,
                          NativeHandle handle,
                          OnReady on_ready,
                          Receiver receiver)
      : IoContextWaitOperationBase<IoContextWatchSignatures, Receiver>(
            context,
            handle,
            std::move(receiver)),
        m_on_ready(std::move(on_ready)) {}

  IoContextWatchOperation(const IoContextWatchOperation&) = delete;
  IoContextWatchOperation& operator=(const IoContextWatchOperation&) = delete;
  IoContextWatchOperation(IoContextWatchOperation&&) = delete;
  IoContextWatchOperation& operator=(IoContextWatchOperation&&) = delete;
  ~IoContextWatchOperation() = default;
};

// The sender `IoContext::watch` returns, which holds @a OnReady until it is
// connected.
template <IoContextOnReady OnReady>
class IoContextWatchSender {
  IoContext* m_context;
  NativeHandle m_handle;
  OnReady m_on_ready;

 public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = IoContextWatchSignatures;

  IoContextWatchSender(IoContext& context,
                       NativeHandle handle,
                       OnReady on_ready)
      : m_context(&context),
        m_handle(handle),
        m_on_ready(std::move(on_ready)) {}

  template <stdexec::receiver_of<completion_signatures> Receiver>
  IoContextWatchOperation<Receiver, OnReady> connect(Receiver receiver) && {
    return IoContextWatchOperation<Receiver, OnReady>(
        *m_context, m_handle, std::move(m_on_ready), std::move(receiver));
  }

  template <stdexec::receiver_of<completion_signatures> Receiver>
    requires std::copy_constructible<OnReady>
  IoContextWatchOperation<Receiver, OnReady> connect(Receiver receiver) const& {
    return IoContextWatchOperation<Receiver, OnReady>(
        *m_context, m_handle, m_on_ready, std::move(receiver));
  }
};

}  // namespace detail

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
    using completion_signatures = detail::IoContextScheduleSignatures;

    explicit Sender(IoContext& context) noexcept : m_context(&context) {}

    template <stdexec::receiver_of<completion_signatures> Receiver>
    detail::IoContextScheduleOperation<Receiver> connect(
        Receiver receiver) const {
      return detail::IoContextScheduleOperation<Receiver>(*m_context,
                                                          std::move(receiver));
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
  using completion_signatures = detail::IoContextWaitSignatures;

  WaitSender(IoContext& context, NativeHandle handle) noexcept
      : m_context(&context), m_handle(handle) {}

  template <stdexec::receiver_of<completion_signatures> Receiver>
  detail::IoContextWaitOperation<Receiver> connect(Receiver receiver) const {
    return detail::IoContextWaitOperation<Receiver>(*m_context, m_handle,
                                                    std::move(receiver));
  }
};

inline IoContext::Scheduler IoContext::get_scheduler() noexcept {
  return Scheduler(*this);
}

inline IoContext::WaitSender IoContext::async_wait(
    NativeHandle handle) noexcept {
  return {*this, handle};
}

template <detail::IoContextOnReady OnReady>
stdexec::sender_of<stdexec::set_value_t(), stdexec::env<>> auto
IoContext::watch(NativeHandle handle, OnReady on_ready) {
  return detail::IoContextWatchSender<OnReady>(*this, handle,
                                               std::move(on_ready));
}

}  // namespace makebelieve
