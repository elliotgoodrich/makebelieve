// SPDX-License-Identifier: MIT
#include "buildcoordinator.hpp"

#include "asyncevent.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace makebelieve {

namespace {

std::error_code canceled() {
  return std::make_error_code(std::errc::operation_canceled);
}

std::error_code dependency_cycle() {
  return std::make_error_code(std::errc::resource_deadlock_would_occur);
}

// Removes the first @a value from @a values, if there.
template <class T>
void erase_one(std::vector<T>& values, const T& value) {
  if (const auto it = std::ranges::find(values, value); it != values.end()) {
    values.erase(it);
  }
}

}  // namespace

// Everything the coordinator tracks, guarded by one lock, and the transitions
// between its states.
class BuildCoordinator::Impl {
 public:
  class Wait;
  class Drain;
  class DrainSender;
  template <class Receiver>
  class DrainOperation;
  struct Effects;

  PermitProvider m_permits;
  Attribution m_attribution;

  // Guards every attempt's and wait's state, and everything below.
  mutable std::mutex m_mutex;

  // Defers completions off stop callbacks before resuming the awaiting task.
  Scheduler m_completions;

  // Attempts whose command is running, by id, for attribution.
  std::unordered_map<AttemptId, Attempt*> m_running;

  // Active waits, for diagnostics.
  std::vector<Wait*> m_waits;

  AttemptId m_last_id = 0;

  Impl(PermitProvider permits, Attribution attribution, Scheduler completions);

  // The coordinator's own operations, as it describes them.
  AttemptPtr create_attempt(AttemptPtr predecessor);
  AttemptSender run(AttemptPtr attempt, BuildSender work);
  void complete(const AttemptPtr& attempt, std::error_code result);
  exec::task<std::error_code> wait(AttemptPtr target,
                                   std::uint32_t requester_pid);
  void cancel_wait(Wait& wait) noexcept;
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::size_t edge_count(const AttemptPtr& from,
                                       const AttemptPtr& to) const;

  // The state transitions, each @pre m_mutex held; what they need done once
  // it is released goes into @a effects.
  void block(Attempt& requester, Effects& effects);
  void give_back(Attempt& attempt);
  void give_back_if_waiting(Attempt& attempt);
  void reacquire(Attempt& requester, Effects& effects);
  void ready(Wait& wait, std::error_code result, Effects& effects);
  void answer(Wait& wait, std::error_code result, Effects& effects);
  void cancel_waits_of(Attempt& attempt, Effects& effects);
  void stop_attempt(Attempt& attempt, Effects& effects);
  void remove_edge(Attempt& from, Attempt& to);
  void unlink(Wait& wait);
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] std::vector<Attempt*> path_to(Attempt& from, Attempt& to);

  void on_permit(Attempt& attempt, PermitLease lease);
  void on_reacquired(Attempt& attempt, bool granted);
  void finish(Attempt& attempt, Drain& drain);

  // Whatever runs @a attempt was stopped: its waits end and any permit being
  // taken back is stopped, as on a cycle, and then its work is. Called within
  // that stop's callback, so it completes nothing itself - a wait woken
  // returns on its own thread, and a take-back stopped completes on its
  // queue's scheduler - and leaves what the attempt waited on running for
  // anyone else.
  void on_outer_stop(Attempt& attempt) noexcept;
};

class BuildCoordinator::Attempt
    : public std::enable_shared_from_this<BuildCoordinator::Attempt> {
 public:
  enum class Phase : std::uint8_t {
    pending,  // not started
    queued,   // started, waiting for its first permit
    running,  // its work is under way
    ended,    // its work has finished
  };
  enum class Permit : std::uint8_t { none, held, released, reacquiring };

  // Registers what the attempt launches under its id.
  struct Registrar {
    Attempt* self;
    std::expected<LaunchRegistration, std::error_code> operator()(
        const LaunchedProcess& process) const {
      return self->coordinator->m_attribution.register_process(self->id,
                                                               process);
    }
  };

  // Stops the attempt, through the coordinator, when whatever runs it is
  // stopped.
  struct ForwardStop {
    Attempt* self;
    void operator()() const noexcept {
      self->coordinator->on_outer_stop(*self);
    }
  };

  Impl* coordinator;
  AttemptId id;

  // Everything below is guarded by the coordinator's m_mutex, except where
  // said.

  Phase phase = Phase::pending;
  Permit permit = Permit::none;

  // Being stopped - on a cycle, or ended - so nothing more is answered for it
  // and it never takes a permit back.
  bool stopping = false;

  // On a cycle: fails with dependency_cycle whatever its work produced.
  bool doomed = false;

  // Set by complete(): what every wait on this attempt gets.
  std::optional<std::error_code> result;

  // The attempt of the same output under way when this was created; cleared
  // once this starts, since it can only start after that one has ended.
  AttemptPtr predecessor;

  // Present while running.
  std::optional<PermitLease> lease;

  // The permit being taken back, if reacquire_outstanding. Once stopping,
  // never replaced, so a stop request made after the lock is released still
  // reaches the one it was meant for.
  std::optional<ReacquireRequest> reacquire;
  bool reacquire_outstanding = false;

  // Completed once the reacquire under way has, when the attempt ends with
  // one outstanding.
  Impl::Drain* drain = nullptr;

  // Waits made by this attempt's requests, and blocked waits on this
  // attempt.
  std::vector<Impl::Wait*> waits_by_me;
  std::vector<Impl::Wait*> waits_on_me;

  // The attempts this one's blocked waits are on, each with how many.
  std::unordered_map<Attempt*, std::size_t> edges_out;

  // Stops the attempt's work. Requested outside the lock.
  stdexec::inplace_stop_source stop;

  // Forwards a stop of whatever runs the attempt. Touched only by the work's
  // own chain, never under the lock.
  std::optional<stdexec::inplace_stop_callback<ForwardStop>> outer_stop;

  Registrar registrar{this};

  Attempt(Impl& owner, AttemptId attempt_id, AttemptPtr before)
      : coordinator(&owner), id(attempt_id), predecessor(std::move(before)) {}

  Attempt(const Attempt&) = delete;
  Attempt& operator=(const Attempt&) = delete;
  Attempt(Attempt&&) = delete;
  Attempt& operator=(Attempt&&) = delete;
  ~Attempt() = default;
};

// One request waiting on an attempt, owned by its suspended coroutine.
class BuildCoordinator::Impl::Wait {
 public:
  enum class State : std::uint8_t {
    blocked,  // waiting on its target
    ready,    // its result is known; waiting for its requester's permit
    done,     // answered, and unlinked from everything
  };

  AttemptPtr target;
  AttemptPtr requester;  // null if unattributed
  State state = State::blocked;
  std::error_code result;
  AsyncEvent completed;
};

// Completed once an ending attempt has nothing left under way.
class BuildCoordinator::Impl::Drain {
 public:
  void (*done)(Drain* self) noexcept;
};

// What a transition needs done once m_mutex is released: stops that may run
// callbacks, permits to take back, which may complete inline, and wakings.
struct BuildCoordinator::Impl::Effects {
  std::vector<AttemptPtr> stop_reacquires;
  std::vector<AttemptPtr> stop_attempts;
  std::vector<AttemptPtr> reacquires;
  std::vector<Drain*> drains;
  std::vector<Wait*> answered;

  // Recursive with on_reacquired: a permit taken back within reacquire()
  // answers, and runs that answer's effects, before this returns. Each round
  // answers a take-back this one started, so it cannot run away.
  // NOLINTNEXTLINE(misc-no-recursion)
  void run(Impl& coordinator) {
    for (const AttemptPtr& attempt : stop_reacquires) {
      if (std::optional<ReacquireRequest>& reacquire = attempt->reacquire;
          reacquire.has_value()) {
        reacquire->stop.request_stop();
      }
    }
    for (const AttemptPtr& attempt : stop_attempts) {
      attempt->stop.request_stop();
    }
    for (const AttemptPtr& attempt : reacquires) {
      try {
        const std::optional<PermitLease>& lease = attempt->lease;
        std::optional<ReacquireRequest>& reacquire = attempt->reacquire;
        if (!lease.has_value() || !reacquire.has_value()) {
          throw std::logic_error("taking back a permit an attempt never had");
        }
        lease->reacquire(*reacquire);
      } catch (...) {
        coordinator.on_reacquired(*attempt, false);
      }
    }
    for (Drain* const drain : drains) {
      drain->done(drain);
    }
    for (Wait* wait : answered) {
      wait->completed.set();
    }
  }
};

BuildCoordinator::Impl::Impl(PermitProvider permits,
                             Attribution attribution,
                             Scheduler completions)
    : m_permits(std::move(permits)),
      m_attribution(attribution),
      m_completions(std::move(completions)) {}

BuildCoordinator::BuildCoordinator(PermitProvider permits,
                                   Attribution attribution,
                                   Scheduler completions)
    : m_impl(std::make_unique<Impl>(std::move(permits),
                                    attribution,
                                    std::move(completions))) {}

BuildCoordinator::~BuildCoordinator() = default;

BuildCoordinator::AttemptPtr BuildCoordinator::create_attempt(
    AttemptPtr predecessor) {
  return m_impl->create_attempt(std::move(predecessor));
}

AttemptSender BuildCoordinator::run(AttemptPtr attempt, BuildSender work) {
  return m_impl->run(std::move(attempt), std::move(work));
}

void BuildCoordinator::complete(const AttemptPtr& attempt,
                                std::error_code result) {
  m_impl->complete(attempt, result);
}

exec::task<std::error_code> BuildCoordinator::wait(
    AttemptPtr target,
    std::uint32_t requester_pid) {
  co_return co_await m_impl->wait(std::move(target), requester_pid);
}

BuildCoordinator::Stats BuildCoordinator::stats() const {
  return m_impl->stats();
}

std::size_t BuildCoordinator::edge_count(const AttemptPtr& from,
                                         const AttemptPtr& to) const {
  return m_impl->edge_count(from, to);
}

AttemptId BuildCoordinator::id_of(const AttemptPtr& attempt) {
  return attempt->id;
}

BuildCoordinator::AttemptPtr BuildCoordinator::Impl::create_attempt(
    AttemptPtr predecessor) {
  const std::lock_guard lock(m_mutex);
  // A predecessor already over cannot hold this one up.
  if (predecessor != nullptr && predecessor->phase == Attempt::Phase::ended) {
    predecessor.reset();
  }
  return std::make_shared<Attempt>(*this, ++m_last_id, std::move(predecessor));
}

void BuildCoordinator::Impl::remove_edge(Attempt& from, Attempt& to) {
  const auto it = from.edges_out.find(&to);
  if (it != from.edges_out.end() && --it->second == 0) {
    from.edges_out.erase(it);
  }
}

void BuildCoordinator::Impl::unlink(Wait& wait) {
  if (wait.requester != nullptr) {
    erase_one(wait.requester->waits_by_me, &wait);
  }
  erase_one(wait.target->waits_on_me, &wait);
  erase_one(m_waits, &wait);
}

void BuildCoordinator::Impl::answer(Wait& wait,
                                    std::error_code result,
                                    Effects& effects) {
  if (wait.state == Wait::State::blocked && wait.requester != nullptr) {
    remove_edge(*wait.requester, *wait.target);
  }
  unlink(wait);
  wait.result = result;
  wait.state = Wait::State::done;
  effects.answered.push_back(&wait);
}

void BuildCoordinator::Impl::give_back(Attempt& attempt) {
  attempt.permit = Attempt::Permit::released;
  // Under the lock, so it cannot race the permit's last release as the
  // attempt ends; it runs nothing on this thread.
  if (attempt.lease.has_value()) {
    attempt.lease->release();
  }
}

void BuildCoordinator::Impl::give_back_if_waiting(Attempt& attempt) {
  if (attempt.permit == Attempt::Permit::held && !attempt.stopping &&
      std::ranges::any_of(attempt.waits_by_me, [](const Wait* wait) {
        return wait->state == Wait::State::blocked;
      })) {
    give_back(attempt);
  }
}

void BuildCoordinator::Impl::block(Attempt& requester, Effects& /*effects*/) {
  // Only a request starting while the permit is held gives it back: one
  // starting while it is being taken back is as though it had started just
  // before, and changes nothing about it - the permit, once back, is given
  // up again for it anyway.
  if (requester.permit == Attempt::Permit::held) {
    give_back(requester);
  }
}

void BuildCoordinator::Impl::reacquire(Attempt& requester, Effects& effects) {
  requester.permit = Attempt::Permit::reacquiring;
  requester.reacquire.emplace(
      [](void* self, bool granted) noexcept {
        auto* const attempt = static_cast<Attempt*>(self);
        attempt->coordinator->on_reacquired(*attempt, granted);
      },
      &requester);
  requester.reacquire_outstanding = true;
  effects.reacquires.push_back(requester.shared_from_this());
}

void BuildCoordinator::Impl::ready(Wait& wait,
                                   std::error_code result,
                                   Effects& effects) {
  Attempt* const requester = wait.requester.get();
  if (requester == nullptr) {
    answer(wait, result, effects);
    return;
  }
  if (requester->stopping) {
    // Its command is going: no permit is taken back just to answer it.
    answer(wait, canceled(), effects);
    return;
  }
  switch (requester->permit) {
    case Attempt::Permit::held:
      answer(wait, result, effects);
      give_back_if_waiting(*requester);
      return;
    case Attempt::Permit::released:
      reacquire(*requester, effects);
      break;
    case Attempt::Permit::reacquiring:
      break;  // shares the take-back under way
    case Attempt::Permit::none:
      answer(wait, canceled(), effects);
      return;
  }
  remove_edge(*requester, *wait.target);
  erase_one(wait.target->waits_on_me, &wait);
  wait.result = result;
  wait.state = Wait::State::ready;
}

// NOLINTNEXTLINE(misc-no-recursion): see Effects::run.
void BuildCoordinator::Impl::on_reacquired(Attempt& attempt, bool granted) {
  Effects effects;
  {
    const std::lock_guard lock(m_mutex);
    attempt.reacquire_outstanding = false;
    attempt.permit =
        granted ? Attempt::Permit::held : Attempt::Permit::released;
    // Everything ready by now is answered together, on the one permit taken
    // back for it.
    const std::vector<Wait*> waits = attempt.waits_by_me;
    for (Wait* const wait : waits) {
      if (wait->state == Wait::State::ready) {
        answer(*wait, granted && !attempt.stopping ? wait->result : canceled(),
               effects);
      }
    }
    // Then, if anything of the attempt still waits on a dependency, the
    // permit goes straight back: the command may be waiting on that request
    // itself - joining the thread that made it, say - and holding the permit
    // could keep the dependency from ever building. It is taken again only
    // once another request is ready, never merely because some still wait,
    // so nothing loops while nothing new is ready.
    give_back_if_waiting(attempt);
    if (attempt.drain != nullptr) {
      effects.drains.push_back(std::exchange(attempt.drain, nullptr));
    }
  }
  effects.run(*this);
}

void BuildCoordinator::Impl::cancel_waits_of(Attempt& attempt,
                                             Effects& effects) {
  const std::vector<Wait*> waits = attempt.waits_by_me;
  for (Wait* const wait : waits) {
    answer(*wait, canceled(), effects);
  }
}

void BuildCoordinator::Impl::stop_attempt(Attempt& attempt, Effects& effects) {
  attempt.stopping = true;
  cancel_waits_of(attempt, effects);
  if (attempt.reacquire_outstanding) {
    effects.stop_reacquires.push_back(attempt.shared_from_this());
  }
  effects.stop_attempts.push_back(attempt.shared_from_this());
}

void BuildCoordinator::Impl::on_outer_stop(Attempt& attempt) noexcept {
  Effects effects;
  {
    const std::lock_guard lock(m_mutex);
    if (attempt.phase == Attempt::Phase::running && !attempt.stopping) {
      stop_attempt(attempt, effects);
    } else {
      effects.stop_attempts.push_back(attempt.shared_from_this());
    }
  }
  effects.run(*this);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::vector<BuildCoordinator::Attempt*> BuildCoordinator::Impl::path_to(
    Attempt& from,
    Attempt& to) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  // Depth-first over what each attempt waits on: its blocked waits' targets,
  // and, not yet started, the attempt it waits behind.
  std::unordered_map<Attempt*, Attempt*> parent{{&from, nullptr}};
  std::vector<Attempt*> stack{&from};
  while (!stack.empty()) {
    Attempt* const node = stack.back();
    stack.pop_back();
    if (node == &to) {
      std::vector<Attempt*> path;
      for (Attempt* it = node; it != nullptr; it = parent[it]) {
        path.push_back(it);
      }
      return path;
    }
    const auto visit = [&](Attempt* next) {
      if (parent.emplace(next, node).second) {
        stack.push_back(next);
      }
    };
    for (const auto& [next, count] : node->edges_out) {
      visit(next);
    }
    if (node->phase == Attempt::Phase::pending &&
        node->predecessor != nullptr) {
      visit(node->predecessor.get());
    }
  }
  return {};
}

exec::task<std::error_code> BuildCoordinator::Impl::wait(
    AttemptPtr target,
    std::uint32_t requester_pid) {
  const auto stop = co_await stdexec::get_stop_token();
  // Outside the lock: it may read /proc or open the requesting process.
  const std::optional<AttemptId> requester_id =
      requester_pid != 0 ? m_attribution.resolve(requester_pid) : std::nullopt;

  Wait wait;
  Effects effects;
  std::unique_lock lock(m_mutex);
  if (stop.stop_requested()) {
    co_return canceled();
  }
  if (const std::optional<std::error_code> result = target->result) {
    co_return *result;
  }
  if (requester_id.has_value()) {
    if (const auto it = m_running.find(*requester_id); it != m_running.end()) {
      wait.requester = it->second->shared_from_this();
    }
  }
  if (wait.requester != nullptr && wait.requester->stopping) {
    co_return canceled();
  }

  if (wait.requester != nullptr) {
    const std::vector<Attempt*> cycle = path_to(*target, *wait.requester);
    if (!cycle.empty()) {
      for (Attempt* const attempt : cycle) {
        // A generation not yet started has run nothing to stop; it runs as
        // usual once the one it waits behind has ended.
        if (attempt->phase == Attempt::Phase::running && !attempt->stopping) {
          attempt->doomed = true;
          stop_attempt(*attempt, effects);
        }
      }
      lock.unlock();
      effects.run(*this);
      co_return dependency_cycle();
    }
  }

  wait.target = target;
  try {
    m_waits.push_back(&wait);
    target->waits_on_me.push_back(&wait);
    if (wait.requester != nullptr) {
      wait.requester->waits_by_me.push_back(&wait);
      ++wait.requester->edges_out[target.get()];
    }
  } catch (...) {
    unlink(wait);
    throw;
  }
  if (wait.requester != nullptr) {
    block(*wait.requester, effects);
  }
  lock.unlock();
  effects.run(*this);

  struct Cancel {
    Impl* owner;
    Wait* wait;
    void operator()() const noexcept { owner->cancel_wait(*wait); }
  };
  // Explicit request cancellation is independent of cancellation of the build:
  // other readers may still need that build.
  const stdexec::inplace_stop_callback<Cancel> cancel(
      stop, Cancel{.owner = this, .wait = &wait});
  co_await stdexec::continues_on(wait.completed.wait(), m_completions);
  co_return wait.result;
}

void BuildCoordinator::Impl::cancel_wait(Wait& wait) noexcept {
  Effects effects;
  {
    const std::lock_guard lock(m_mutex);
    if (wait.state != Wait::State::done) {
      answer(wait, canceled(), effects);
      // An interrupted syscall can resume its command immediately. Restore its
      // accounting when the last wait leaves, without delaying cancellation
      // (in particular, mount shutdown) behind another command's permit.
      Attempt* const requester = wait.requester.get();
      if (requester != nullptr && !requester->stopping &&
          requester->waits_by_me.empty() &&
          requester->permit == Attempt::Permit::released) {
        reacquire(*requester, effects);
      }
    }
  }
  effects.run(*this);
}

void BuildCoordinator::Impl::complete(const AttemptPtr& attempt,
                                      std::error_code result) {
  Effects effects;
  {
    const std::lock_guard lock(m_mutex);
    attempt->result = result;
    attempt->predecessor.reset();
    const std::vector<Wait*> waits = attempt->waits_on_me;
    for (Wait* const wait : waits) {
      ready(*wait, result, effects);
    }
  }
  effects.run(*this);
}

void BuildCoordinator::Impl::on_permit(Attempt& attempt, PermitLease lease) {
  const std::lock_guard lock(m_mutex);
  attempt.phase = Attempt::Phase::running;
  attempt.permit = Attempt::Permit::held;
  attempt.lease.emplace(lease);
  m_running.emplace(attempt.id, &attempt);
}

void BuildCoordinator::Impl::finish(Attempt& attempt, Drain& drain) {
  Effects effects;
  {
    const std::lock_guard lock(m_mutex);
    // From here it can be neither doomed nor attributed to.
    attempt.phase = Attempt::Phase::ended;
    attempt.stopping = true;
    m_running.erase(attempt.id);
    cancel_waits_of(attempt, effects);
    if (attempt.reacquire_outstanding) {
      // Its lease must outlive the take-back, which is stopped and waited
      // for.
      attempt.drain = &drain;
      effects.stop_reacquires.push_back(attempt.shared_from_this());
    } else {
      effects.drains.push_back(&drain);
    }
  }
  effects.run(*this);
}

BuildCoordinator::Stats BuildCoordinator::Impl::stats() const {
  const std::lock_guard lock(m_mutex);
  Stats stats;
  stats.waits = m_waits.size();
  std::unordered_set<Attempt*> seen;
  for (const Wait* const wait : m_waits) {
    for (Attempt* const attempt : {wait->requester.get(), wait->target.get()}) {
      if (attempt != nullptr && seen.insert(attempt).second) {
        stats.edges += attempt->edges_out.size();
      }
    }
  }
  for (const auto& [id, attempt] : m_running) {
    if (seen.insert(attempt).second) {
      stats.edges += attempt->edges_out.size();
    }
    stats.held += attempt->permit == Attempt::Permit::held ? 1 : 0;
    stats.reacquiring +=
        attempt->permit == Attempt::Permit::reacquiring ? 1 : 0;
  }
  return stats;
}

std::size_t BuildCoordinator::Impl::edge_count(const AttemptPtr& from,
                                               const AttemptPtr& to) const {
  const std::lock_guard lock(m_mutex);
  const auto it = from->edges_out.find(to.get());
  return it == from->edges_out.end() ? 0 : it->second;
}

// Completes once the attempt it is started for has nothing left under way.
template <class Receiver>
class BuildCoordinator::Impl::DrainOperation : public Drain {
  Receiver m_receiver;
  Impl* m_coordinator;
  Attempt* m_attempt;

 public:
  using operation_state_concept = stdexec::operation_state_t;

  DrainOperation(Receiver receiver, Impl& coordinator, Attempt& attempt)
      : Drain{[](Drain* self) noexcept {
          stdexec::set_value(
              std::move(static_cast<DrainOperation*>(self)->m_receiver));
        }},
        m_receiver(std::move(receiver)),
        m_coordinator(&coordinator),
        m_attempt(&attempt) {}

  DrainOperation(const DrainOperation&) = delete;
  DrainOperation& operator=(const DrainOperation&) = delete;
  DrainOperation(DrainOperation&&) = delete;
  DrainOperation& operator=(DrainOperation&&) = delete;
  ~DrainOperation() = default;

  void start() & noexcept { m_coordinator->finish(*m_attempt, *this); }
};

// Ends an attempt whose work has finished: once nothing of it remains under
// way - no permit being taken back - it completes.
class BuildCoordinator::Impl::DrainSender {
  Impl* m_coordinator;
  Attempt* m_attempt;

 public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t()>;

  DrainSender(Impl& coordinator, Attempt& attempt) noexcept
      : m_coordinator(&coordinator), m_attempt(&attempt) {}

  template <class Receiver>
  [[nodiscard]] DrainOperation<Receiver> connect(Receiver receiver) const {
    return DrainOperation<Receiver>(std::move(receiver), *m_coordinator,
                                    *m_attempt);
  }
};

AttemptSender BuildCoordinator::Impl::run(AttemptPtr attempt,
                                          BuildSender work) {
  {
    const std::lock_guard lock(m_mutex);
    attempt->phase = Attempt::Phase::queued;
    // It is started only once the attempt it waited behind has ended.
    attempt->predecessor.reset();
  }
  return m_permits([this, attempt = std::move(attempt), work = std::move(work)](
                       PermitLease lease) mutable -> AttemptSender {
    on_permit(*attempt, lease);
    return stdexec::read_env(stdexec::get_stop_token) |
           stdexec::let_value([this, attempt, work = std::move(work)](
                                  stdexec::inplace_stop_token outer) mutable {
             attempt->outer_stop.emplace(outer,
                                         Attempt::ForwardStop{attempt.get()});
             return stdexec::write_env(
                        std::move(work),
                        stdexec::env{stdexec::prop{stdexec::get_stop_token,
                                                   attempt->stop.get_token()},
                                     stdexec::prop{get_launch_registrar,
                                                   LaunchRegistrar(
                                                       attempt->registrar)}}) |
                    stdexec::upon_error(
                        [](const std::exception_ptr& error) noexcept {
                          return BuildResult(
                              std::unexpected(to_error_code(error)));
                        }) |
                    stdexec::upon_stopped([]() noexcept {
                      return BuildResult(std::unexpected(canceled()));
                    }) |
                    stdexec::let_value([this, attempt](BuildResult& result) {
                      return DrainSender(*this, *attempt) |
                             stdexec::then([&result, attempt]() noexcept {
                               // Nothing can stop it now, and the stop it
                               // forwards from may soon be gone.
                               attempt->outer_stop.reset();
                               // Ended, so this can no longer change.
                               if (attempt->doomed) {
                                 return BuildResult(
                                     std::unexpected(dependency_cycle()));
                               }
                               return std::move(result);
                             });
                    });
           });
  });
}

}  // namespace makebelieve
