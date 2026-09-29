// SPDX-License-Identifier: MIT
#include "buildcoordinator.hpp"

#include "builddirectorytree.hpp"
#include "buildpermits.hpp"
#include "buildqueue.hpp"
#include "commandrunner.hpp"
#include "directorytreeutil.hpp"
#include "inmemorydirectorytree.hpp"
#include "launchregistrar.hpp"

#include <gtest/gtest.h>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace makebelieve;
using namespace std::chrono_literals;

namespace ex = stdexec;

// How long anything here waits before deciding it never will.
constexpr auto k_patience = 10s;

std::vector<std::string> split(std::string_view text, std::string_view by) {
  std::vector<std::string> parts;
  while (true) {
    const std::size_t at = text.find(by);
    std::string_view part = text.substr(0, at);
    while (!part.empty() && part.front() == ' ') {
      part.remove_prefix(1);
    }
    while (!part.empty() && part.back() == ' ') {
      part.remove_suffix(1);
    }
    if (!part.empty()) {
      parts.emplace_back(part);
    }
    if (at == std::string_view::npos) {
      return parts;
    }
    text.remove_prefix(at + by.size());
  }
}

// Tells attempts apart by the fake pids their fake commands register, the way
// ProcessAttribution does by real ones. Registration can be made to fail.
class FakeAttribution {
  std::mutex m_mutex;
  std::map<std::uint32_t, AttemptId> m_attempts;
  bool m_fail_next = false;

 public:
  std::expected<LaunchRegistration, std::error_code> register_process(
      AttemptId attempt,
      const LaunchedProcess& process) {
    const std::lock_guard lock(m_mutex);
    if (std::exchange(m_fail_next, false)) {
      return std::unexpected(std::make_error_code(std::errc::no_such_process));
    }
    m_attempts.emplace(process.pid, attempt);
    return LaunchRegistration([this, pid = process.pid] {
      const std::lock_guard lock(m_mutex);
      m_attempts.erase(pid);
    });
  }

  std::optional<AttemptId> resolve(std::uint32_t requester) {
    const std::lock_guard lock(m_mutex);
    const auto it = m_attempts.find(requester);
    return it == m_attempts.end() ? std::nullopt
                                  : std::optional<AttemptId>(it->second);
  }

  void fail_next() {
    const std::lock_guard lock(m_mutex);
    m_fail_next = true;
  }
};

// A BuildQueue whose leases count how often they are given back and taken
// again.
template <class Queue>
class CountingQueue {
  Queue* m_queue;

  template <class Lease>
  struct CountingLease {
    Lease* lease;
    CountingQueue* counts;

    void release() {
      ++counts->releases;
      lease->release();
    }

    auto reacquire() {
      ++counts->reacquires;
      return lease->reacquire();
    }
  };

 public:
  std::atomic<int> releases = 0;
  std::atomic<int> reacquires = 0;

  explicit CountingQueue(Queue& queue) : m_queue(&queue) {}

  template <class MakeWork>
  auto schedule_leased(MakeWork make) {
    return m_queue->schedule_leased(
        [this, make = std::move(make)](auto& lease) mutable {
          using Lease = std::remove_reference_t<decltype(lease)>;
          return ex::just(CountingLease<Lease>{&lease, this}) |
                 ex::let_value([&make](CountingLease<Lease>& counting) {
                   return make(counting);
                 });
        });
  }
};

// Fake commands, each a little script run on threads of its own:
//   `open X`    opens output X through the tree, as the command
//   `wait S`    waits for signal S (or for the command to be stopped)
//   `signal S`  raises signal S
//   `hang`      waits until the command is stopped
//   `fail`      makes the command fail
// Steps are separated by `;`, threads by `|`. The output a command builds is a
// transcript of what its opens returned, like `b=ok c=E36 `.
class FakeCommands {
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::set<std::string> m_signals;
  std::map<std::string, int> m_runs;
  int m_active = 0;  // attempts with a thread outside an open
  int m_most_active = 0;
  int m_stopped = 0;  // commands that saw a stop
  std::atomic<std::uint32_t> m_last_pid = 1000;

 public:
  FakeAttribution attribution;
  const BuildDirectoryTree* tree = nullptr;
  BuildCoordinator* coordinator = nullptr;
  std::size_t concurrency = 0;
  std::atomic<bool> held_within_limit = true;

  // One command running: its threads, how many of them are outside an open,
  // and whether it has been stopped.
  struct Run {
    FakeCommands* commands;
    std::string output;
    std::uint32_t pid;
    int active_threads = 0;
    bool stopped = false;
    // Told by an open that it is being stopped: answered without a permit,
    // as a command on its way out is, so no longer counted as active.
    bool dying = false;
    bool failed = false;
    std::string transcript;
  };

  void check_held() {
    if (coordinator != nullptr && coordinator->stats().held > concurrency) {
      held_within_limit = false;
    }
  }

  // A thread of @a run starts or resumes (+1) or stops or blocks (-1).
  void activity(Run& run, int change) {
    {
      const std::lock_guard lock(m_mutex);
      if (run.dying) {
        return;
      }
      const bool was = run.active_threads > 0;
      run.active_threads += change;
      const bool is = run.active_threads > 0;
      if (!was && is) {
        m_most_active = std::max(m_most_active, ++m_active);
      } else if (was && !is) {
        --m_active;
      }
    }
    check_held();
  }

  void raise(const std::string& name) {
    {
      const std::lock_guard lock(m_mutex);
      m_signals.insert(name);
    }
    m_changed.notify_all();
  }

  // Waits for @a name, or for @a run to be stopped; false if neither came.
  bool await(Run& run, const std::string& name) {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, k_patience, [&] {
      return run.stopped || (!name.empty() && m_signals.contains(name));
    });
  }

  void stop(Run& run) {
    {
      const std::lock_guard lock(m_mutex);
      if (!run.stopped) {
        run.stopped = true;
        ++m_stopped;
      }
    }
    m_changed.notify_all();
  }

  void run_thread(Run& run, const std::string& steps) {
    activity(run, +1);
    for (const std::string& step : split(steps, ";")) {
      if (step.starts_with("open ")) {
        const std::string target = step.substr(5);
        activity(run, -1);
        const std::expected<FileInfo, std::error_code> opened =
            makebelieve::DirectoryTreeUtil::open(
                *tree, target, OpenContext{.requester_pid = run.pid});
        if (!opened.has_value() &&
            (opened.error() == std::errc::operation_canceled ||
             opened.error() == std::errc::resource_deadlock_would_occur)) {
          const std::lock_guard lock(m_mutex);
          if (!run.dying && run.active_threads > 0) {
            --m_active;
          }
          run.dying = true;
        }
        activity(run, +1);
        const std::lock_guard lock(m_mutex);
        run.transcript += target + "=" +
                          (opened.has_value()
                               ? std::string("ok")
                               : "E" + std::to_string(opened.error().value())) +
                          " ";
      } else if (step.starts_with("wait ")) {
        await(run, step.substr(5));
      } else if (step.starts_with("signal ")) {
        raise(step.substr(7));
      } else if (step == "hang") {
        await(run, "");
      } else if (step == "fail") {
        const std::lock_guard lock(m_mutex);
        run.failed = true;
      }
    }
    activity(run, -1);
  }

  // Runs @a script as @a run's command, registering it first, and returns
  // what it built.
  BuildResult execute(Run& run,
                      const std::string& script,
                      LaunchRegistrar registrar) {
    std::expected<LaunchRegistration, std::error_code> registration =
        registrar(LaunchedProcess{.pid = run.pid});
    if (!registration.has_value()) {
      return std::unexpected(registration.error());
    }
    std::vector<std::thread> threads;
    for (const std::string& steps : split(script, "|")) {
      threads.emplace_back([this, &run, steps] { run_thread(run, steps); });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
    const std::lock_guard lock(m_mutex);
    if (run.failed) {
      return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    return BuildOutput{.bytes = run.transcript, .inputs = {}};
  }

  template <class Receiver>
  class Operation {
    struct OnStop {
      Operation* self;
      void operator()() const noexcept {
        self->m_run.commands->stop(self->m_run);
      }
    };

    Receiver m_receiver;
    Run m_run;
    std::string m_script;

   public:
    using operation_state_concept = ex::operation_state_t;

    Operation(Receiver receiver,
              FakeCommands& commands,
              std::string output,
              std::string script)
        : m_receiver(std::move(receiver)),
          m_run{.commands = &commands,
                .output = std::move(output),
                .pid = ++commands.m_last_pid},
          m_script(std::move(script)) {}

    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&&) = delete;
    Operation& operator=(Operation&&) = delete;
    ~Operation() = default;

    void start() & noexcept {
      {
        const std::lock_guard lock(m_run.commands->m_mutex);
        ++m_run.commands->m_runs[m_run.output];
      }
      // Its own thread, detached: it completes from there, never from within
      // a stop callback, and touches nothing of this once it has.
      std::thread([this] {
        BuildResult result = [&] {
          const ex::inplace_stop_callback<OnStop> on_stop(
              ex::get_stop_token(ex::get_env(m_receiver)), OnStop{this});
          return m_run.commands->execute(
              m_run, m_script, get_launch_registrar(ex::get_env(m_receiver)));
        }();
        ex::set_value(std::move(m_receiver), std::move(result));
      }).detach();
    }
  };

  struct Sender {
    using sender_concept = ex::sender_t;
    using completion_signatures =
        ex::completion_signatures<ex::set_value_t(BuildResult)>;

    FakeCommands* commands;
    std::string output;
    std::string script;

    template <class Receiver>
    [[nodiscard]] Operation<Receiver> connect(Receiver receiver) const {
      return Operation<Receiver>(std::move(receiver), *commands, output,
                                 script);
    }
  };

  CommandRunner runner() {
    return [this](const Command& command) -> BuildSender {
      return Sender{.commands = this,
                    .output = command.output.generic_string(),
                    .script = command.text};
    };
  }

  int runs(const std::string& output) {
    const std::lock_guard lock(m_mutex);
    return m_runs[output];
  }

  int most_active() {
    const std::lock_guard lock(m_mutex);
    return m_most_active;
  }

  int stopped() {
    const std::lock_guard lock(m_mutex);
    return m_stopped;
  }
};

// What an open returned: ok, or the error's value.
std::string outcome(const std::expected<FileInfo, std::error_code>& opened) {
  return opened.has_value() ? "ok"
                            : "E" + std::to_string(opened.error().value());
}

std::string error_of(std::errc error) {
  return "E" + std::to_string(std::make_error_code(error).value());
}

using PoolQueue = BuildQueue<exec::static_thread_pool::scheduler>;

// A tree of fake commands, capped at @a concurrency attempts holding a permit.
class Fixture {
 public:
  exec::static_thread_pool pool{2};
  PoolQueue queue;
  CountingQueue<PoolQueue> counting{queue};
  FakeCommands commands;
  BuildCoordinator coordinator;
  InMemoryDirectoryTree source;
  std::optional<BuildDirectoryTree> tree;

  // Whether no more than the cap of attempts ever had a thread running
  // outside an open. Holds only for single-threaded commands: a thread of an
  // attempt whose other thread gave its permit back runs on without one -
  // the permits are cooperative accounting.
  bool check_active = true;

  explicit Fixture(std::size_t concurrency)
      : queue(concurrency, pool.get_scheduler()),
        coordinator(permits_from(counting),
                    Attribution(commands.attribution),
                    pool.get_scheduler()) {
    commands.coordinator = &coordinator;
    commands.concurrency = concurrency;
  }

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&&) = delete;
  Fixture& operator=(Fixture&&) = delete;

  ~Fixture() {
    tree.reset();
    // Nothing held or waiting is left behind.
    const BuildCoordinator::Stats stats = coordinator.stats();
    EXPECT_EQ(stats.waits, 0U);
    EXPECT_EQ(stats.edges, 0U);
    EXPECT_EQ(stats.held, 0U);
    EXPECT_TRUE(commands.held_within_limit);
    if (check_active) {
      EXPECT_LE(commands.most_active(), static_cast<int>(commands.concurrency));
    }
  }

  // Declares @a manifest and builds the tree over it.
  void declare(std::string_view manifest) {
    source.write_file("build.makebelieve", manifest);
    tree.emplace(source, commands.runner(), coordinator);
    commands.tree = &*tree;
  }

  void redeclare(std::string_view manifest) {
    source.write_file("build.makebelieve", manifest);
  }

  // Opens @a output on no build's behalf.
  std::expected<FileInfo, std::error_code> open(const std::string& output) {
    return makebelieve::DirectoryTreeUtil::open(tree.value(), output,
                                                OpenContext{});
  }

  // Opens @a output on another thread, returning its outcome when asked.
  class Opening {
    std::mutex m_mutex;
    std::condition_variable m_done;
    std::optional<std::string> m_outcome;
    // Last, so everything the thread touches exists before it starts.
    std::thread m_thread;

   public:
    Opening(Fixture& fixture, std::string output)
        : m_thread([this, &fixture, output = std::move(output)] {
            std::string result = outcome(fixture.open(output));
            {
              const std::lock_guard lock(m_mutex);
              m_outcome = std::move(result);
            }
            m_done.notify_all();
          }) {}

    Opening(const Opening&) = delete;
    Opening& operator=(const Opening&) = delete;
    Opening(Opening&&) = delete;
    Opening& operator=(Opening&&) = delete;

    ~Opening() {
      if (m_thread.joinable()) {
        m_thread.join();
      }
    }

    // The outcome, waiting (bounded) for it.
    std::string get() {
      std::unique_lock lock(m_mutex);
      if (!m_done.wait_for(lock, k_patience,
                           [this] { return m_outcome.has_value(); })) {
        return "timed out";
      }
      return m_outcome.value();
    }

    [[nodiscard]] bool done() {
      const std::lock_guard lock(m_mutex);
      return m_outcome.has_value();
    }
  };

  // The bytes @a output holds now.
  std::string content(const std::string& output) {
    const auto info = tree.value().status(output);
    if (!info.has_value()) {
      return "missing";
    }
    return makebelieve::DirectoryTreeUtil::read(tree.value(), output, 0,
                                                std::get<FileInfo>(*info).size)
        .value_or("unreadable");
  }

  // Waits (bounded) until @a condition holds of the coordinator's stats.
  bool until(const std::function<bool(const BuildCoordinator::Stats&)>&
                 condition) const {
    const auto deadline = std::chrono::steady_clock::now() + k_patience;
    while (!condition(coordinator.stats())) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }
};

TEST(BuildCoordinator, AtTheCapEachAttemptOpeningAnUnbuiltOutputCompletes) {
  Fixture fixture(2);
  fixture.declare(
      "@/a1 = run open b1\n"
      "@/a2 = run open b2\n"
      "@/b1 = run signal b1\n"
      "@/b2 = run signal b2\n");
  Fixture::Opening a1(fixture, "a1");
  Fixture::Opening a2(fixture, "a2");
  EXPECT_EQ(a1.get(), "ok");
  EXPECT_EQ(a2.get(), "ok");
  EXPECT_EQ(fixture.content("a1"), "b1=ok ");
  EXPECT_EQ(fixture.content("a2"), "b2=ok ");
}

TEST(BuildCoordinator, AChainLongerThanTheCapCompletes) {
  Fixture fixture(1);
  fixture.declare(
      "@/a = run open b\n"
      "@/b = run open c\n"
      "@/c = run signal c\n");
  EXPECT_EQ(outcome(fixture.open("a")), "ok");
  EXPECT_EQ(fixture.content("a"), "b=ok ");
  EXPECT_EQ(fixture.content("b"), "c=ok ");
}

TEST(BuildCoordinator, AttemptsOpeningOneUnbuiltOutputShareItsOneBuild) {
  Fixture fixture(3);
  fixture.declare(
      "@/a1 = run open b\n"
      "@/a2 = run open b\n"
      "@/a3 = run open b\n"
      "@/b = run wait go\n");
  Fixture::Opening a1(fixture, "a1");
  Fixture::Opening a2(fixture, "a2");
  Fixture::Opening a3(fixture, "a3");
  ASSERT_TRUE(fixture.until([](const auto& stats) {
    return stats.waits >= 6;  // three on the a's, three on b
  }));
  fixture.commands.raise("go");
  EXPECT_EQ(a1.get(), "ok");
  EXPECT_EQ(a2.get(), "ok");
  EXPECT_EQ(a3.get(), "ok");
  EXPECT_EQ(fixture.commands.runs("b"), 1);
}

// One thread's open must be answered while another of the same attempt still
// waits, or neither ever returns: c finishes only once a has seen b.
TEST(BuildCoordinator, AReadyOpenIsAnsweredWhileAnotherOfItsAttemptWaits) {
  Fixture fixture(2);
  fixture.check_active = false;
  fixture.declare(
      "@/a = run open c | open b; signal seen_b\n"
      "@/b = run signal b\n"
      "@/c = run wait seen_b\n");
  EXPECT_EQ(outcome(fixture.open("a")), "ok");
  const std::string transcript = fixture.content("a");
  EXPECT_NE(transcript.find("b=ok"), std::string::npos);
  EXPECT_NE(transcript.find("c=ok"), std::string::npos);
}

// Two opens of one attempt made ready by the same build take the permit back
// once between them.
TEST(BuildCoordinator, OpensReadyTogetherShareOneReacquisition) {
  Fixture fixture(1);
  fixture.check_active = false;
  fixture.declare(
      "@/a = run open b | open b\n"
      "@/b = run wait go\n");
  Fixture::Opening a(fixture, "a");
  ASSERT_TRUE(fixture.until([](const auto& stats) {
    return stats.waits >= 3 && stats.edges == 1;  // one shared a->b edge
  }));
  fixture.commands.raise("go");
  EXPECT_EQ(a.get(), "ok");
  EXPECT_EQ(fixture.content("a"), "b=ok b=ok ");
  EXPECT_EQ(fixture.counting.reacquires.load(), 1);
  EXPECT_EQ(fixture.counting.releases.load(), 1);
}

// An open that starts to wait while its attempt holds the permit gives it back
// again, however many times: at a cap of one, b and c could not otherwise run.
TEST(BuildCoordinator, EachNewWaitWhileHoldingGivesThePermitBack) {
  Fixture fixture(1);
  fixture.declare(
      "@/a = run open b; open c\n"
      "@/b = run signal b\n"
      "@/c = run signal c\n");
  EXPECT_EQ(outcome(fixture.open("a")), "ok");
  EXPECT_EQ(fixture.content("a"), "b=ok c=ok ");
  EXPECT_EQ(fixture.counting.releases.load(), 2);
  EXPECT_EQ(fixture.counting.reacquires.load(), 2);
}

// At a cap of one, a command whose second thread opens c while its permit is
// being taken back for the first thread's open of b, and which then joins
// both threads: once b's open is answered, the permit goes straight back,
// since c's open still waits - and c needs it. Holding on would deadlock.
TEST(BuildCoordinator, APermitTakenBackIsGivenUpAgainWhileOpensStillWait) {
  Fixture fixture(1);
  fixture.check_active = false;
  fixture.declare(
      "@/a = run open b | wait t2; open c\n"
      "@/b = run wait b_go\n"
      "@/c = run signal c\n"
      "@/y = run wait y_go\n");
  Fixture::Opening a(fixture, "a");
  // a's first thread waits on b, which holds the only permit.
  ASSERT_TRUE(fixture.until([](const auto& stats) {
    return stats.waits == 2 && stats.edges == 1 && stats.held == 1;
  }));
  // y queues for a permit, so b's goes to y when b ends, before a asks for
  // one back.
  Fixture::Opening y(fixture, "y");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.waits == 3; }));
  fixture.commands.raise("b_go");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.reacquiring == 1; }));
  const int releases = fixture.counting.releases.load();
  EXPECT_EQ(releases, 1);

  // Mid-reacquisition: a's second thread waits on c, which queues behind a's
  // take-back, and nothing is given back yet.
  fixture.commands.raise("t2");
  ASSERT_TRUE(fixture.until([](const auto& stats) {
    return stats.edges == 1 && stats.reacquiring == 1;
  }));
  EXPECT_EQ(fixture.counting.releases.load(), releases);

  // y ends: a takes the permit back, answers b's open, and - c's open still
  // waiting - gives it straight up again, so c builds and a finishes, joining
  // both threads.
  fixture.commands.raise("y_go");
  EXPECT_EQ(a.get(), "ok");
  EXPECT_EQ(y.get(), "ok");
  EXPECT_EQ(fixture.content("a"), "b=ok c=ok ");
  EXPECT_EQ(fixture.counting.releases.load(), releases + 1);
  EXPECT_EQ(fixture.counting.reacquires.load(), 2);
}

// Several opens of one attempt still waiting neither give the permit back
// more than once nor have it taken back while nothing is ready: it is taken
// back only for an answer, and given up again only while others still wait.
TEST(BuildCoordinator, WaitingOpensNeitherDoubleReleaseNorSpin) {
  Fixture fixture(1);
  fixture.check_active = false;
  fixture.declare(
      "@/a = run open b | open c | open d\n"
      "@/b = run wait go\n"
      "@/c = run wait go\n"
      "@/d = run wait go\n");
  Fixture::Opening a(fixture, "a");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.edges == 3; }));
  // Nothing is ready: one release, and no take-back, however long it waits.
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(fixture.counting.releases.load(), 1);
  EXPECT_EQ(fixture.counting.reacquires.load(), 0);

  fixture.commands.raise("go");
  EXPECT_EQ(a.get(), "ok");
  // Each take-back answered something, and each but the last was followed by
  // exactly one release - the first release was the first open's own.
  const int reacquires = fixture.counting.reacquires.load();
  EXPECT_GE(reacquires, 1);
  EXPECT_LE(reacquires, 3);
  EXPECT_EQ(fixture.counting.releases.load(), reacquires);
}

// Two opens from one attempt on the same output: the edge between them lasts
// until neither waits. Here the second joins the next build, so the first
// finishing leaves it.
TEST(BuildCoordinator, AnEdgeOutlivesTheFirstOfItsWaitsToFinish) {
  Fixture fixture(3);
  fixture.check_active = false;
  fixture.declare(
      "@/a = run open b | wait again; open b\n"
      "@/b = run wait b1\n");
  Fixture::Opening a(fixture, "a");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.edges == 1; }));
  // b changes mid-build, so a's second open waits on the next build of b.
  fixture.redeclare(
      "@/a = run open b | wait again; open b\n"
      "@/b = run wait b2\n");
  fixture.commands.raise("again");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.edges == 2; }));
  fixture.commands.raise("b1");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.edges == 1; }));
  fixture.commands.raise("b2");
  EXPECT_EQ(a.get(), "ok");
}

TEST(BuildCoordinator, AnAttemptOpeningItsOwnOutputIsACycle) {
  Fixture fixture(2);
  // It ignores the failure, and would succeed.
  fixture.declare("@/a = run open a\n");
  EXPECT_EQ(outcome(fixture.open("a")),
            error_of(std::errc::resource_deadlock_would_occur));
  // Never published: still the placeholder.
  EXPECT_EQ(fixture.content("a"), std::string(1, '\0'));
}

TEST(BuildCoordinator, ATwoAttemptCycleStopsBothAndPublishesNeither) {
  Fixture fixture(2);
  // b ignores the failed open and never exits of its own accord.
  fixture.declare(
      "@/a = run open b\n"
      "@/b = run open a; hang\n");
  EXPECT_EQ(outcome(fixture.open("a")),
            error_of(std::errc::resource_deadlock_would_occur));
  EXPECT_EQ(outcome(fixture.open("b")),
            error_of(std::errc::resource_deadlock_would_occur));
  EXPECT_EQ(fixture.content("a"), std::string(1, '\0'));
  EXPECT_EQ(fixture.content("b"), std::string(1, '\0'));
  EXPECT_GE(fixture.commands.stopped(), 1);
}

TEST(BuildCoordinator, AThreeAttemptCycleStopsAllOfThem) {
  Fixture fixture(1);
  fixture.declare(
      "@/a = run open b\n"
      "@/b = run open c; hang\n"
      "@/c = run open a\n");
  Fixture::Opening a(fixture, "a");
  EXPECT_EQ(a.get(), error_of(std::errc::resource_deadlock_would_occur));
  // The cycle's attempts all failed, and a retry of any of them is another
  // cycle, never a hang.
  for (const char* output : {"b", "c"}) {
    EXPECT_EQ(outcome(fixture.open(output)),
              error_of(std::errc::resource_deadlock_would_occur))
        << output;
  }
  for (const char* output : {"a", "b", "c"}) {
    EXPECT_EQ(fixture.content(output), std::string(1, '\0')) << output;
  }
}

TEST(BuildCoordinator, AnUnattributedOpenWaitsWithoutHoldingAPermit) {
  Fixture fixture(1);
  fixture.declare("@/b = run wait go\n");
  Fixture::Opening b(fixture, "b");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.waits == 1; }));
  // b holds the only permit; the open holds none.
  EXPECT_EQ(fixture.coordinator.stats().held, 1U);
  fixture.commands.raise("go");
  EXPECT_EQ(b.get(), "ok");
}

TEST(BuildCoordinator, AnOpenSeesItsDependencysFailure) {
  Fixture fixture(1);
  fixture.declare(
      "@/a = run open b\n"
      "@/b = run fail\n");
  EXPECT_EQ(outcome(fixture.open("a")), "ok");
  EXPECT_EQ(fixture.content("a"), "b=" + error_of(std::errc::io_error) + " ");
}

TEST(BuildCoordinator, ACommandThatCannotBeRegisteredNeverRunsAndFails) {
  Fixture fixture(1);
  fixture.declare("@/a = run signal ran\n");
  fixture.commands.attribution.fail_next();
  EXPECT_EQ(outcome(fixture.open("a")), error_of(std::errc::no_such_process));
  EXPECT_EQ(fixture.content("a"), std::string(1, '\0'));
}

// An attempt stopped - here, found on a cycle by its own second thread - just
// as the output it waits on completes, over and over: nothing is leaked or
// doubled, and the output still completes for the others waiting on it.
TEST(BuildCoordinator, CancellationRacingCompletionLeaksNothing) {
  for (int round = 0; round < 25; ++round) {
    Fixture fixture(2);
    fixture.check_active = false;
    fixture.declare(
        "@/a = run open b | wait go; open a\n"
        "@/b = run wait go\n");
    Fixture::Opening a(fixture, "a");
    ASSERT_TRUE(
        fixture.until([](const auto& stats) { return stats.edges == 1; }));
    Fixture::Opening b(fixture, "b");
    ASSERT_TRUE(
        fixture.until([](const auto& stats) { return stats.waits >= 3; }));
    fixture.commands.raise("go");
    EXPECT_EQ(a.get(), error_of(std::errc::resource_deadlock_would_occur));
    EXPECT_EQ(b.get(), "ok");
    EXPECT_EQ(fixture.content("b"), "");
  }
}

// An open waiting on a build when its output changes gets that build's
// result; an open made after the change waits for the next build, and never
// gets the earlier one's.
TEST(BuildCoordinator, EachOpenGetsTheResultOfTheGenerationItJoined) {
  Fixture fixture(2);
  fixture.declare("@/b = run wait k; fail\n");
  Fixture::Opening first(fixture, "b");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.waits == 1; }));

  fixture.redeclare("@/b = run wait k1\n");
  Fixture::Opening second(fixture, "b");
  ASSERT_TRUE(
      fixture.until([](const auto& stats) { return stats.waits == 2; }));

  fixture.commands.raise("k");
  EXPECT_EQ(first.get(), error_of(std::errc::io_error));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(second.done());

  fixture.commands.raise("k1");
  EXPECT_EQ(second.get(), "ok");
  EXPECT_EQ(fixture.commands.runs("b"), 2);
}

// Many consumers join the same build without a dispatcher admission budget.
TEST(BuildCoordinator, ConcurrentWaitersShareOneBuild) {
  Fixture fixture(1);
  fixture.declare("@/b = run wait go\n");
  std::vector<std::unique_ptr<Fixture::Opening>> readers;
  readers.reserve(80);
  for (int i = 0; i < 80; ++i) {
    readers.push_back(std::make_unique<Fixture::Opening>(fixture, "b"));
  }
  const auto deadline = std::chrono::steady_clock::now() + k_patience;
  while (fixture.coordinator.stats().waits != 80U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(fixture.coordinator.stats().waits, 80U);
  fixture.commands.raise("go");
  for (const auto& reader : readers) {
    EXPECT_EQ(reader->get(), "ok");
  }
  EXPECT_EQ(fixture.commands.runs("b"), 1);
}

// A stop of whatever runs an attempt - its caller cancelling it, say - ends
// the attempt's dependency waits at once, rather than leaving its command
// blocked until what it waits on happens to finish; and what it waited on
// goes on building for anyone else waiting on it.
TEST(BuildCoordinator, StoppingAnAttemptEndsItsWaitsButNotItsDependencies) {
  Fixture fixture(2);
  fixture.declare("@/b = run wait go\n");
  const BuildCoordinator::AttemptPtr attempt =
      fixture.coordinator.create_attempt();
  ex::inplace_stop_source stop;
  std::promise<void> ended;
  const std::future<void> ending = ended.get_future();
  std::thread running([&] {
    static_cast<void>(ex::sync_wait(ex::write_env(
        fixture.coordinator.run(attempt, fixture.commands.runner()(Command{
                                             .output = "a", .text = "open b"})),
        ex::prop{ex::get_stop_token, stop.get_token()})));
    ended.set_value();
  });
  // The attempt waits on b, and so does someone else.
  const bool waiting =
      fixture.until([](const auto& stats) { return stats.edges == 1; });
  Fixture::Opening other(fixture, "b");
  const bool shared =
      fixture.until([](const auto& stats) { return stats.waits == 2; });

  // Bounded well within b's own patience, after which b would finish anyway
  // and release the attempt regardless.
  stop.request_stop();
  const bool ended_alone = ending.wait_for(2s) == std::future_status::ready;

  // Let b finish either way, so a failure cannot wedge the teardown.
  fixture.commands.raise("go");
  running.join();
  EXPECT_TRUE(waiting);
  EXPECT_TRUE(shared);
  EXPECT_TRUE(ended_alone) << "the stopped attempt waited on b regardless";
  EXPECT_EQ(other.get(), "ok");
  EXPECT_EQ(fixture.commands.runs("b"), 1);
}

TEST(BuildCoordinator, InterruptedRequestRestoresItsCommandsPermit) {
  Fixture fixture(1);
  // These fake commands wait outside file opens while this test drives their
  // requests explicitly, so the fake active-thread count does not apply.
  fixture.check_active = false;
  fixture.declare("@/a = run wait finish_a\n@/b = run wait finish_b\n");
  Fixture::Opening a(fixture, "a");
  // The first fake command registers pid 1001 before executing its script.
  EXPECT_TRUE(fixture.until([&](const auto& stats) {
    return stats.held == 1 &&
           fixture.commands.attribution.resolve(1001).has_value();
  }));
  auto target = fixture.coordinator.create_attempt();
  ex::inplace_stop_source stop;
  std::promise<std::error_code> result;
  auto canceled = result.get_future();
  std::thread reader([&] {
    result.set_value(std::get<0>(*ex::sync_wait(
        ex::write_env(fixture.coordinator.wait(target, 1001),
                      ex::prop{ex::get_stop_token, stop.get_token()}) |
        ex::upon_stopped([] {
          return std::make_error_code(std::errc::operation_canceled);
        }))));
  });
  EXPECT_TRUE(
      fixture.until([](const auto& stats) { return stats.edges == 1; }));
  Fixture::Opening b(fixture, "b");
  EXPECT_TRUE(fixture.until([&](const auto& stats) {
    return stats.held == 1 && fixture.commands.runs("b") == 1;
  }));

  stop.request_stop();
  const bool promptly = canceled.wait_for(2s) == std::future_status::ready;
  EXPECT_TRUE(fixture.until([](const auto& stats) {
    return stats.edges == 0 && stats.reacquiring == 1;
  }));
  fixture.commands.raise("finish_b");
  EXPECT_EQ(b.get(), "ok");
  EXPECT_TRUE(fixture.until([](const auto& stats) {
    return stats.held == 1 && stats.reacquiring == 0;
  }));
  fixture.commands.raise("finish_a");
  EXPECT_EQ(a.get(), "ok");
  reader.join();
  EXPECT_TRUE(promptly);
  EXPECT_EQ(canceled.get(), std::errc::operation_canceled);
  EXPECT_EQ(fixture.counting.reacquires, 1);
}

// Canceling one suspended request leaves other consumers and the target intact.
TEST(BuildCoordinator, CancelingARequestDetachesOnlyThatWaiter) {
  exec::static_thread_pool pool(1);
  BuildCoordinator coordinator(unlimited_permits(), Attribution::none(),
                               pool.get_scheduler());
  auto target = coordinator.create_attempt();
  stdexec::counting_scope scope;
  stdexec::inplace_stop_source stop;
  std::error_code first;
  std::error_code second;
  stdexec::spawn(
      stdexec::starts_on(
          pool.get_scheduler(),
          stdexec::write_env(
              coordinator.wait(target),
              stdexec::prop{stdexec::get_stop_token, stop.get_token()})) |
          stdexec::upon_stopped([] {
            return std::make_error_code(std::errc::operation_canceled);
          }) |
          stdexec::then(
              [&](std::error_code result) noexcept { first = result; }) |
          stdexec::upon_error(
              [](const std::exception_ptr&) noexcept { ADD_FAILURE(); }),
      scope.get_token());
  stdexec::spawn(
      stdexec::starts_on(pool.get_scheduler(), coordinator.wait(target)) |
          stdexec::then(
              [&](std::error_code result) noexcept { second = result; }) |
          stdexec::upon_error(
              [](const std::exception_ptr&) noexcept { ADD_FAILURE(); }),
      scope.get_token());
  const auto deadline = std::chrono::steady_clock::now() + k_patience;
  while (coordinator.stats().waits < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(coordinator.stats().waits, 2U);
  stop.request_stop();
  EXPECT_EQ(coordinator.stats().waits, 1U);
  coordinator.complete(target, {});
  stdexec::sync_wait(scope.join());
  EXPECT_EQ(first, std::errc::operation_canceled);
  EXPECT_FALSE(second);
  EXPECT_EQ(coordinator.stats().waits, 0U);
}

}  // namespace
