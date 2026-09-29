// SPDX-License-Identifier: MIT
#pragma once

#include "commandrunner.hpp"

#include <exec/any_sender_of.hpp>
#include <stdexec/execution.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <utility>

// How a `BuildCoordinator` sees the permits that cap how many build attempts
// run at once: any provider of leases that can be given back and taken again,
// such as a `BuildQueue`, type-erased so the coordinator depends on none.

namespace makebelieve {

/// A build attempt as the coordinator runs it: completes with its result, or
/// stopped if stopped before it started, or with an exception.
using AttemptSender = exec::any_sender<exec::any_receiver<
    stdexec::completion_signatures<stdexec::set_value_t(BuildResult),
                                   stdexec::set_error_t(std::exception_ptr),
                                   stdexec::set_stopped_t()>,
    exec::queries<stdexec::inplace_stop_token(
        stdexec::get_stop_token_t) noexcept>>>;

/// @class ReacquireRequest
/// One request to take a permit back: stoppable through @link stop, and
/// answered - exactly once, never within the call that stopped it - through
/// the callback it was made with.
class ReacquireRequest {
  void (*m_on_done)(void* context, bool granted) noexcept;
  void* m_context;

 public:
  /// Stops the request; it then completes, not granted, unless it had
  /// already been granted.
  stdexec::inplace_stop_source stop;

  /// Calls @a on_done with @a context once answered.
  ReacquireRequest(void (*on_done)(void* context, bool granted) noexcept,
                   void* context) noexcept
      : m_on_done(on_done), m_context(context) {}

  ReacquireRequest(const ReacquireRequest&) = delete;
  ReacquireRequest& operator=(const ReacquireRequest&) = delete;
  ReacquireRequest(ReacquireRequest&&) = delete;
  ReacquireRequest& operator=(ReacquireRequest&&) = delete;
  /// Destroys an answered request; does not cancel a pending reacquisition.
  /// @pre No callback or permit operation still refers to this request.
  ~ReacquireRequest() = default;

  /// Answers the request: with a permit held again if @a granted.
  void complete(bool granted) noexcept { m_on_done(m_context, granted); }
};

/// @class PermitLease
/// A non-owning reference to one build attempt's lease on a permit, as a
/// `BuildQueue::Lease` provides: it can give its permit back and take one
/// again. Valid until the attempt's work completes.
class PermitLease {
  struct Operations {
    void (*release)(void* lease) noexcept;
    void (*reacquire)(void* lease, ReacquireRequest& request);
  };

  // Runs a lease's reacquire() sender, heap-allocated since the coordinator
  // cannot know its size, answering @a request when it completes.
  template <class Lease>
  class Reacquire {
    struct Receiver {
      using receiver_concept = stdexec::receiver_t;
      Reacquire* self;
      ReacquireRequest* request;

      void set_value() noexcept { self->finish(true); }
      void set_stopped() noexcept { self->finish(false); }

      [[nodiscard]] auto get_env() const noexcept {
        return stdexec::prop{stdexec::get_stop_token,
                             request->stop.get_token()};
      }
    };

    ReacquireRequest* m_request;
    stdexec::connect_result_t<decltype(std::declval<Lease&>().reacquire()),
                              Receiver>
        m_operation;

    // Frees this before answering, so an answer that goes on to take another
    // permit back never finds the previous one still here. Nothing touches
    // the operation once it has completed, so freeing it from within its own
    // completion is safe.
    void finish(bool granted) noexcept {
      ReacquireRequest* const request = m_request;
      delete this;
      request->complete(granted);
    }

   public:
    Reacquire(Lease& lease, ReacquireRequest& request)
        : m_request(&request),
          m_operation(
              stdexec::connect(lease.reacquire(), Receiver{this, &request})) {}

    Reacquire(const Reacquire&) = delete;
    Reacquire& operator=(const Reacquire&) = delete;
    Reacquire(Reacquire&&) = delete;
    Reacquire& operator=(Reacquire&&) = delete;
    ~Reacquire() = default;

    void start() noexcept { stdexec::start(m_operation); }
  };

  // One table per Lease type, built at compile time.
  template <class Lease>
  static const Operations* operations_for() noexcept {
    static constexpr Operations operations{
        .release =
            [](void* lease) noexcept { static_cast<Lease*>(lease)->release(); },
        .reacquire =
            [](void* lease, ReacquireRequest& request) {
              (new Reacquire<Lease>(*static_cast<Lease*>(lease), request))
                  ->start();
            },
    };
    return &operations;
  }

  // Stands in for a lease on a provider with no limit.
  static constexpr Operations k_unlimited{
      .release = [](void*) noexcept {},
      .reacquire = [](void*,
                      ReacquireRequest& request) { request.complete(true); },
  };

  void* m_lease;
  const Operations* m_operations;

  PermitLease(void* lease, const Operations* operations) noexcept
      : m_lease(lease), m_operations(operations) {}

 public:
  /// Refers to @a lease, which gives its permit back through `release()` and
  /// takes one again through the sender `reacquire()` returns.
  template <class Lease>
    requires(!std::same_as<std::remove_cv_t<Lease>, PermitLease>) &&
            requires(Lease& lease) {
              lease.release();
              { lease.reacquire() } -> stdexec::sender;
            }
  explicit PermitLease(Lease& lease) noexcept
      : PermitLease(&lease, operations_for<Lease>()) {}

  /// A lease on no limit at all: giving it back does nothing, and taking it
  /// back is granted at once.
  [[nodiscard]] static PermitLease unlimited() noexcept {
    return {nullptr, &k_unlimited};
  }

  /// Gives the permit back. Nothing waiting for it runs on this thread, so
  /// this may be called while holding a lock that such work would take.
  void release() const noexcept { m_operations->release(m_lease); }

  /// Takes a permit back, answering @a request - within this call if one is
  /// free, otherwise later on another thread.
  /// @pre The permit is not held, and no other reacquire is under way.
  void reacquire(ReacquireRequest& request) const {
    m_operations->reacquire(m_lease, request);
  }
};

/// Runs the work its argument makes, given a lease on a permit, once a permit
/// is free; completes as that work does, the permit given back by then.
using PermitProvider = std::function<AttemptSender(
    std::move_only_function<AttemptSender(PermitLease)> make_work)>;

/// A provider whose permits come from @a queue - a `BuildQueue`, or anything
/// else whose `schedule_leased` behaves like it.
/// @pre @a queue outlives the provider and everything it runs.
template <class Queue>
[[nodiscard]] PermitProvider permits_from(Queue& queue) {
  return [&queue](std::move_only_function<AttemptSender(PermitLease)> make)
             -> AttemptSender {
    return queue.schedule_leased([make = std::move(make)](auto& lease) mutable {
      return make(PermitLease(lease));
    });
  };
}

/// A provider with no limit: every attempt starts at once.
[[nodiscard]] inline PermitProvider unlimited_permits() {
  return [](std::move_only_function<AttemptSender(PermitLease)> make)
             -> AttemptSender {
    return stdexec::just() |
           stdexec::let_value([make = std::move(make)]() mutable {
             return make(PermitLease::unlimited());
           });
  };
}

}  // namespace makebelieve
