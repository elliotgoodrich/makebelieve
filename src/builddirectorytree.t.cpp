// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "inmemorydirectorytree.hpp"
#include "iocontext.hpp"
#include "realdirectorytree.hpp"
#include "tracer.hpp"

#include <exec/single_thread_context.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace makebelieve;

// The path a command echoed back, without the newline (and, under cmd, the
// carriage return) that echoing appended.
std::filesystem::path trimmed_path(std::string_view echoed) {
  const std::size_t end = echoed.find_last_not_of("\r\n");
  return echoed.substr(0, end == std::string_view::npos ? 0 : end + 1);
}

// The tests here care about an output's bytes, not its traced inputs, so wrap a
// plain string as a successful BuildResult carrying no inputs.
BuildDirectoryTree::BuildResult built(std::string bytes) {
  return BuildDirectoryTree::BuildOutput{.bytes = std::move(bytes),
                                         .inputs = {}};
}

// A build that has already finished with @a result, completing as soon as it
// is started.
BuildDirectoryTree::BuildSender finished(
    BuildDirectoryTree::BuildResult result) {
  return stdexec::just(std::move(result));
}

// A runner that produces fixed bytes for any command, so the tree's behaviour
// can be tested without a shell.
BuildDirectoryTree::CommandRunner returning(std::string output) {
  return [output = std::move(output)](BuildDirectoryTree::Command /*command*/) {
    return finished(built(output));
  };
}

// A runner that reports a fixed set of traced @a inputs alongside its output,
// and whose output ("build N") reflects how many times it has run, so a rebuild
// is observable. @a runs is bumped on every invocation.
BuildDirectoryTree::CommandRunner counting_with_inputs(
    int& runs,
    std::vector<std::filesystem::path> inputs) {
  return [&runs,
          inputs = std::move(inputs)](BuildDirectoryTree::Command /*command*/) {
    ++runs;
    return finished(BuildDirectoryTree::BuildOutput{
        .bytes = "build " + std::to_string(runs), .inputs = inputs});
  };
}

// A thread-safe runner that holds each build in flight until the test
// completes it. A build stopped while held completes stopped instead, from a
// thread of the runner's own, since a build must not complete from within its
// stop callback.
class DeferredRunner {
  // A build being held, as the test sees it.
  class Pending {
   public:
    virtual void finish(BuildDirectoryTree::BuildResult result) noexcept = 0;

   protected:
    Pending() = default;
    ~Pending() = default;

   public:
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;
    Pending(Pending&&) = delete;
    Pending& operator=(Pending&&) = delete;
  };

  struct State {
    std::mutex mutex;
    std::condition_variable started;
    std::deque<Pending*> pending;
    int runs = 0;
    int stopped = 0;

    // Where stopped builds complete, owned by the DeferredRunner.
    exec::single_thread_context* stopper = nullptr;
    stdexec::counting_scope* stopping = nullptr;
  };

  template <class Receiver>
  class Operation final : public Pending {
    struct OnStop {
      Operation* self;
      void operator()() const noexcept { self->on_stop(); }
    };
    using Token = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

    Receiver m_receiver;
    std::shared_ptr<State> m_state;

    // Set when a stop arrives before start() has queued this build, which
    // start() then completes stopped itself.
    bool m_stopped_early = false;
    std::optional<stdexec::stop_callback_for_t<Token, OnStop>> m_on_stop;

   public:
    using operation_state_concept = stdexec::operation_state_t;

    Operation(Receiver receiver, std::shared_ptr<State> state)
        : m_receiver(std::move(receiver)), m_state(std::move(state)) {}

    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&&) = delete;
    Operation& operator=(Operation&&) = delete;
    ~Operation() = default;

    void start() & noexcept {
      // Registered before this build is queued, so a stop arriving now - even
      // one that runs the callback inline, right here - finds nothing queued to
      // complete and leaves that to the block below.
      m_on_stop.emplace(stdexec::get_stop_token(stdexec::get_env(m_receiver)),
                        OnStop{this});
      bool stopped = false;
      {
        const std::lock_guard lock(m_state->mutex);
        ++m_state->runs;
        stopped = m_stopped_early;
        if (stopped) {
          ++m_state->stopped;
        } else {
          m_state->pending.push_back(this);
        }
      }
      m_state->started.notify_all();
      if (stopped) {
        complete_stopped();
      }
    }

    void finish(BuildDirectoryTree::BuildResult result) noexcept override {
      stdexec::set_value(std::move(m_receiver), std::move(result));
    }

   private:
    // Completes this build stopped if it is still queued; if complete() took
    // it first, completing it is complete()'s job.
    void on_stop() noexcept {
      {
        const std::lock_guard lock(m_state->mutex);
        const auto it = std::ranges::find(m_state->pending, this);
        if (it == m_state->pending.end()) {
          m_stopped_early = true;
          return;
        }
        m_state->pending.erase(it);
        ++m_state->stopped;
      }
      complete_stopped();
    }

    void complete_stopped() noexcept {
      stdexec::spawn(stdexec::schedule(m_state->stopper->get_scheduler()) |
                         stdexec::then([this]() noexcept {
                           stdexec::set_stopped(std::move(m_receiver));
                         }),
                     m_state->stopping->get_token());
    }
  };

  struct Sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(
                                           BuildDirectoryTree::BuildResult),
                                       stdexec::set_stopped_t()>;

    std::shared_ptr<State> state;

    template <class Receiver>
    Operation<Receiver> connect(Receiver receiver) && {
      return Operation<Receiver>(std::move(receiver), std::move(state));
    }
  };

  exec::single_thread_context m_stopper;
  stdexec::counting_scope m_stopping;
  std::shared_ptr<State> m_state = std::make_shared<State>();

 public:
  DeferredRunner() {
    m_state->stopper = &m_stopper;
    m_state->stopping = &m_stopping;
  }

  ~DeferredRunner() { stdexec::sync_wait(m_stopping.join()); }

  DeferredRunner(const DeferredRunner&) = delete;
  DeferredRunner& operator=(const DeferredRunner&) = delete;
  DeferredRunner(DeferredRunner&&) = delete;
  DeferredRunner& operator=(DeferredRunner&&) = delete;

  [[nodiscard]] BuildDirectoryTree::CommandRunner runner() const {
    return [state = m_state](BuildDirectoryTree::Command /*command*/) {
      return BuildDirectoryTree::BuildSender(Sender{state});
    };
  }

  // Waits (bounded) until @a count builds have started.
  [[nodiscard]] bool wait_for_runs(int count) const {
    std::unique_lock lock(m_state->mutex);
    return m_state->started.wait_for(lock, std::chrono::seconds(10),
                                     [&] { return m_state->runs >= count; });
  }

  // Completes the oldest pending build.
  void complete(BuildDirectoryTree::BuildResult result) const {
    Pending* pending = nullptr;
    {
      const std::lock_guard lock(m_state->mutex);
      ASSERT_FALSE(m_state->pending.empty());
      pending = m_state->pending.front();
      m_state->pending.pop_front();
    }
    // Unlocked: completing may start the next build.
    pending->finish(std::move(result));
  }

  [[nodiscard]] int runs() const {
    const std::lock_guard lock(m_state->mutex);
    return m_state->runs;
  }

  // How many builds were stopped rather than completed.
  [[nodiscard]] int stopped() const {
    const std::lock_guard lock(m_state->mutex);
    return m_state->stopped;
  }
};

class BuildDirectoryTreeTest : public ::testing::Test {
 protected:
  InMemoryDirectoryTree source;

  // Where the shell runner's commands run, and what a real source is watched
  // through. Declared before (and so outliving) every tree a test makes.
  exec::static_thread_pool pool{2};
  IoContext io;

  // A real, throwaway directory for the tests that drive the real shell runner.
  std::filesystem::path work =
      std::filesystem::temp_directory_path() /
      ("makebelieve-test-" +
       std::string(
           ::testing::UnitTest::GetInstance()->current_test_info()->name()));

  void SetUp() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
  }

  void write_manifest(std::string_view text) {
    source.write_file("build.makebelieve", text);
  }

  void write_input(std::string_view name, std::string_view content) {
    std::ofstream stream(work / name, std::ios::binary);
    stream << content;
  }

  static std::size_t output_size(const BuildDirectoryTree& tree,
                                 const std::filesystem::path& path) {
    const auto info = tree.status(path);
    EXPECT_TRUE(info.has_value());
    return std::get<FileInfo>(*info).size;
  }

  // Opens (which builds) then reads a whole output.
  static std::string read_output(const BuildDirectoryTree& tree,
                                 const std::filesystem::path& path) {
    const std::expected<FileInfo, std::error_code> opened = tree.open(path);
    EXPECT_TRUE(opened.has_value());
    if (!opened.has_value()) {
      return {};
    }
    const auto content = tree.read(path, 0, opened->size);
    EXPECT_TRUE(content.has_value());
    return content.value_or(std::string{});
  }
};

TEST_F(BuildDirectoryTreeTest, NoManifestYieldsAnEmptyTree) {
  int runs = 0;
  const BuildDirectoryTree tree(source, [&runs](BuildDirectoryTree::Command) {
    ++runs;
    return finished(built(std::string{}));
  });

  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  EXPECT_TRUE(root->empty());
  EXPECT_EQ(runs, 0);
}

// Only open() builds; until then an output reports size 1.
TEST_F(BuildDirectoryTreeTest, OnlyOpenBuilds) {
  write_manifest("@/output.txt = run anything %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source, [&runs](BuildDirectoryTree::Command) {
    ++runs;
    return finished(built("built"));
  });

  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  ASSERT_EQ(root->size(), 1U);
  EXPECT_EQ((*root)[0].name, "output.txt");
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);
  EXPECT_TRUE(tree.read("output.txt", 0, 16).has_value());
  EXPECT_EQ(runs, 0);

  const std::expected<FileInfo, std::error_code> opened =
      tree.open("output.txt");
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->size, 5U);
  EXPECT_EQ(runs, 1);
}

TEST_F(BuildDirectoryTreeTest, OpenOfAnythingButAnOutputIsNotABuild) {
  write_manifest("@/out/file.txt = run anything %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source, counting_with_inputs(runs, {}));

  EXPECT_EQ(tree.open("out").error(), std::errc::is_a_directory);
  EXPECT_EQ(tree.open("missing.txt").error(),
            std::errc::no_such_file_or_directory);
  EXPECT_EQ(runs, 0);
}

TEST_F(BuildDirectoryTreeTest, ReadHandsTheRawCommandToTheRunner) {
  write_manifest("@/output.txt = run cp input.txt %out\n");

  std::vector<std::string> commands;
  const BuildDirectoryTree tree(
      source, [&commands](BuildDirectoryTree::Command command) {
        commands.push_back(std::move(command.text));
        return finished(built("result"));
      });

  EXPECT_EQ(read_output(tree, "output.txt"), "result");
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0], "cp input.txt %out");  // %out is the runner's concern
}

// A `copy` rule reaches the runner as an action and the path it names, with
// no shell command anywhere in sight.
TEST_F(BuildDirectoryTreeTest, ReadHandsACopysSourceToTheRunner) {
  write_manifest("@/output.txt = copy src/input.txt\n");

  std::vector<BuildDirectoryTree::Command> commands;
  const BuildDirectoryTree tree(
      source, [&commands](BuildDirectoryTree::Command command) {
        commands.push_back(std::move(command));
        return finished(built("result"));
      });

  EXPECT_EQ(read_output(tree, "output.txt"), "result");
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].action, Manifest::Action::Copy);
  EXPECT_EQ(commands[0].text, "src/input.txt");
}

// A command also names the output it is building, so a runner can give the
// scratch file it hands to `%out` the same name.
TEST_F(BuildDirectoryTreeTest, ReadHandsTheOutputBeingBuiltToTheRunner) {
  write_manifest("@/out/report.pdf = run build %out\n");

  std::vector<BuildDirectoryTree::Command> commands;
  const BuildDirectoryTree tree(
      source, [&commands](BuildDirectoryTree::Command command) {
        commands.push_back(std::move(command));
        return finished(built("result"));
      });

  EXPECT_EQ(read_output(tree, "out/report.pdf"), "result");
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].output, std::filesystem::path("out/report.pdf"));
}

TEST_F(BuildDirectoryTreeTest, BuildsLazilyAndOncePerOutputOnSuccess) {
  write_manifest("@/output.txt = run build %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source, [&runs](BuildDirectoryTree::Command) {
    ++runs;
    return finished(built("hello world"));
  });

  EXPECT_EQ(runs, 0);
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
  EXPECT_EQ(output_size(tree, "output.txt"), 11U);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
  EXPECT_EQ(runs, 1);  // not rebuilt
}

TEST_F(BuildDirectoryTreeTest, RetriesAfterAFailedBuild) {
  write_manifest("@/output.txt = run build %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source, [&runs](BuildDirectoryTree::Command) {
    ++runs;
    if (runs == 1) {
      return finished(
          std::unexpected(std::make_error_code(std::errc::io_error)));
    } else {
      return finished(built("recovered"));
    }
  });

  // The failed first build fails the open...
  const std::expected<FileInfo, std::error_code> failed =
      tree.open("output.txt");
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), std::errc::io_error);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);

  // ...so a later open tries again, and this time it sticks.
  EXPECT_EQ(read_output(tree, "output.txt"), "recovered");
  EXPECT_EQ(runs, 2);
  EXPECT_EQ(read_output(tree, "output.txt"), "recovered");
  EXPECT_EQ(runs, 2);
}

TEST_F(BuildDirectoryTreeTest, OnlyGeneratedOutputsAreVisible) {
  // The source files - the manifest included - are not part of the output
  // namespace; only the declared `@/` outputs are.
  source.write_file("input.txt", "some input");
  write_manifest("@/output.txt = run build %out\n");

  const BuildDirectoryTree tree(source, returning("built"));

  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  ASSERT_EQ(root->size(), 1U);
  EXPECT_EQ((*root)[0].name, "output.txt");
  EXPECT_FALSE(tree.status("input.txt").has_value());
  EXPECT_FALSE(tree.status("build.makebelieve").has_value());
}

TEST_F(BuildDirectoryTreeTest, CreatesParentDirectoriesForNestedOutputs) {
  write_manifest("@/out/deep/file.txt = run build %out\n");

  const BuildDirectoryTree tree(source, returning("nested"));

  const auto out = tree.status("out");
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(std::holds_alternative<DirectoryInfo>(*out));
  EXPECT_EQ(read_output(tree, "out/deep/file.txt"), "nested");
}

// A runner may return any sender that fits, without naming BuildSender.
TEST_F(BuildDirectoryTreeTest, RunnerCanReturnAnyFittingSender) {
  write_manifest("@/output.txt = run build %out\n");

  const BuildDirectoryTree tree(source, [](BuildDirectoryTree::Command) {
    return stdexec::just(std::string("generic")) |
           stdexec::then([](std::string bytes) { return built(bytes); });
  });

  EXPECT_EQ(read_output(tree, "output.txt"), "generic");
}

// A runner that throws, or a build that completes with an exception, fails the
// build rather than wedging the open waiting on it.
TEST_F(BuildDirectoryTreeTest, ARunnerOrBuildThatThrowsFailsTheBuild) {
  write_manifest(
      "@/thrown.txt = run build %out\n@/raised.txt = run build %out\n");

  const BuildDirectoryTree tree(
      source,
      [](BuildDirectoryTree::Command command)
          -> BuildDirectoryTree::BuildSender {
        if (command.output == "thrown.txt") {
          throw std::system_error(
              std::make_error_code(std::errc::permission_denied));
        }
        return stdexec::just() |
               stdexec::then([]() -> BuildDirectoryTree::BuildResult {
                 throw std::system_error(
                     std::make_error_code(std::errc::bad_message));
               });
      });

  EXPECT_EQ(tree.open("thrown.txt").error(), std::errc::permission_denied);
  EXPECT_EQ(tree.open("raised.txt").error(), std::errc::bad_message);
}

// open() waits for the build to report back; status() does not.
TEST_F(BuildDirectoryTreeTest, OpenBlocksUntilTheBuildReportsBack) {
  write_manifest("@/output.txt = run build %out\n");

  const DeferredRunner builds;
  const BuildDirectoryTree tree(source, builds.runner());

  std::atomic<bool> returned = false;
  std::string content;
  std::thread reader([&] {
    content = read_output(tree, "output.txt");
    returned = true;
  });

  ASSERT_TRUE(builds.wait_for_runs(1));
  // Its build is still pending.
  EXPECT_FALSE(returned);
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);

  builds.complete(built("done later"));
  reader.join();
  EXPECT_EQ(content, "done later");
  EXPECT_EQ(output_size(tree, "output.txt"), 10U);
  EXPECT_EQ(builds.runs(), 1);
}

// Two opens of one unbuilt output share one build.
TEST_F(BuildDirectoryTreeTest, ConcurrentOpensShareOneBuild) {
  write_manifest("@/output.txt = run build %out\n");

  const DeferredRunner builds;
  const BuildDirectoryTree tree(source, builds.runner());

  std::string first;
  std::string second;
  std::thread first_reader([&] { first = read_output(tree, "output.txt"); });
  ASSERT_TRUE(builds.wait_for_runs(1));
  std::thread second_reader([&] { second = read_output(tree, "output.txt"); });

  builds.complete(built("shared"));
  first_reader.join();
  second_reader.join();
  EXPECT_EQ(first, "shared");
  EXPECT_EQ(second, "shared");
  EXPECT_EQ(builds.runs(), 1);
}

// Destroying the tree stops the builds under way and waits for them, rather
// than leaving them to complete into a tree that is gone.
TEST_F(BuildDirectoryTreeTest, DestructionStopsBuildsUnderWay) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt = run build %out\n");

  const DeferredRunner builds;
  {
    const BuildDirectoryTree tree(source, builds.runner());
    std::thread first(
        [&] { EXPECT_EQ(read_output(tree, "output.txt"), "v1"); });
    ASSERT_TRUE(builds.wait_for_runs(1));
    builds.complete(BuildDirectoryTree::BuildOutput{.bytes = "v1",
                                                    .inputs = {"input.txt"}});
    first.join();

    // An eager rebuild, left in flight.
    source.write_file("input.txt", "v2");
    ASSERT_TRUE(builds.wait_for_runs(2));
    EXPECT_EQ(builds.stopped(), 0);
  }

  EXPECT_EQ(builds.stopped(), 1);
}

// The bundled shell runner actually drives the system shell end to end.
// `cmake -E copy` runs the same under either shell, and cmake is on PATH
// wherever ctest is.
TEST_F(BuildDirectoryTreeTest, ShellRunnerBuildsLazilyThroughTheShell) {
  write_input("input.txt", "hello world");
  write_manifest("@/output.txt = run cmake -E copy input.txt %out\n");

  const BuildDirectoryTree tree(
      source, BuildDirectoryTree::shell_runner(work, pool.get_scheduler()));

  EXPECT_EQ(output_size(tree, "output.txt"), 1U);  // unbuilt until opened
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
  EXPECT_EQ(output_size(tree, "output.txt"), 11U);
}

// The scratch file `%out` names has the file name of the output being built,
// so a tool that picks its format from the extension (pandoc and friends)
// needs no extra flag. `cmake -E echo` prints the path it was handed under
// either shell, redirected into that same file so it becomes the output.
TEST_F(BuildDirectoryTreeTest, ShellRunnerGivesOutTheOutputsName) {
  write_manifest("@/docs/report.pdf = run cmake -E echo %out > %out\n");

  const BuildDirectoryTree tree(
      source, BuildDirectoryTree::shell_runner(work, pool.get_scheduler()));

  const std::filesystem::path scratch =
      trimmed_path(read_output(tree, "docs/report.pdf"));
  EXPECT_EQ(scratch.filename(), "report.pdf");
}

// An output with no extension leaves the scratch file without one either,
// rather than inventing something for a tool to sniff.
TEST_F(BuildDirectoryTreeTest, ShellRunnerAddsNoExtensionToABareOutput) {
  write_manifest("@/report = run cmake -E echo %out > %out\n");

  const BuildDirectoryTree tree(
      source, BuildDirectoryTree::shell_runner(work, pool.get_scheduler()));

  const std::filesystem::path scratch =
      trimmed_path(read_output(tree, "report"));
  EXPECT_EQ(scratch.filename(), "report");
}

// A `capture` rule has no output file: the bytes the command writes to
// standard output are the output. `cmake -E cat` writes its input there
// verbatim, under either shell.
TEST_F(BuildDirectoryTreeTest, ShellRunnerCapturesStandardOutput) {
  write_input("input.txt", "hello world");
  write_manifest("@/output.txt = capture cmake -E cat input.txt\n");

  const BuildDirectoryTree tree(
      source, BuildDirectoryTree::shell_runner(work, pool.get_scheduler()));

  EXPECT_EQ(output_size(tree, "output.txt"), 1U);  // unbuilt until opened
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
}

// A `copy` rule runs no command at all: the output is the named file's bytes,
// read straight out of the working directory, and it stays lazy like any other.
TEST_F(BuildDirectoryTreeTest, ShellRunnerCopiesAFileWithoutAShell) {
  write_input("input.txt", "hello world");
  write_manifest("@/output.txt = copy input.txt\n");

  const BuildDirectoryTree tree(
      source, BuildDirectoryTree::shell_runner(work, pool.get_scheduler()));

  EXPECT_EQ(output_size(tree, "output.txt"), 1U);  // unbuilt until opened
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
  EXPECT_EQ(output_size(tree, "output.txt"), 11U);
}

// The copied bytes are taken verbatim, so a binary source survives intact.
TEST_F(BuildDirectoryTreeTest, ShellRunnerCopiesBytesVerbatim) {
  const std::string binary("a\0b\r\nc", 6);
  write_input("input.bin", binary);
  write_manifest("@/output.bin = copy input.bin\n");

  const BuildDirectoryTree tree(
      source, BuildDirectoryTree::shell_runner(work, pool.get_scheduler()));

  EXPECT_EQ(read_output(tree, "output.bin"), binary);
}

// A copy whose source is not there fails the open, with the reason, rather
// than quietly producing nothing.
TEST_F(BuildDirectoryTreeTest, ShellRunnerFailsACopyOfAMissingFile) {
  write_manifest("@/output.txt = copy missing.txt\n");

  const BuildDirectoryTree tree(
      source, BuildDirectoryTree::shell_runner(work, pool.get_scheduler()));

  const std::expected<FileInfo, std::error_code> opened =
      tree.open("output.txt");
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(), std::errc::no_such_file_or_directory);

  // Still unbuilt, so a later open - once the file is there - tries again.
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);
  write_input("missing.txt", "here now");
  EXPECT_EQ(read_output(tree, "output.txt"), "here now");
}

// ---------------------------------------------------------------------------
// Rebuilding when a traced input changes.
// ---------------------------------------------------------------------------

// Once an output has been read, a change to one of the inputs its build traced
// rebuilds it eagerly - the new content is in place before anyone reads again.
TEST_F(BuildDirectoryTreeTest, RebuildsAReadOutputWhenATracedInputChanges) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt = run build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));

  // The first read builds it lazily and records input.txt as a dependency.
  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");
  EXPECT_EQ(runs, 1);

  // Changing that input rebuilds the output without a new read...
  source.write_file("input.txt", "v2");
  EXPECT_EQ(runs, 2);

  // ...and the fresh content is already in place for the next read.
  EXPECT_EQ(read_output(tree, "output.txt"), "build 2");
  EXPECT_EQ(runs, 2);  // that read did not trigger yet another build
}

// The eager rebuild's new content reaches the tree's own subscribers, so a
// downstream watcher learns the output changed.
TEST_F(BuildDirectoryTreeTest, AnEagerRebuildNotifiesSubscribers) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt = run build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));
  read_output(tree, "output.txt");  // build and materialise it

  std::vector<std::filesystem::path> changed;
  const Subscription subscription =
      tree.subscribe_to_changes([&changed](const DirectoryTreeDiff& diff) {
        changed.insert(changed.end(), diff.entries_changed.begin(),
                       diff.entries_changed.end());
      });

  source.write_file("input.txt", "v2");  // eager rebuild -> notification

  EXPECT_EQ(runs, 2);
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0], std::filesystem::path("output.txt"));
}

// A change to a file the output never read leaves it alone.
TEST_F(BuildDirectoryTreeTest, IgnoresChangesToUntrackedFiles) {
  source.write_file("input.txt", "v1");
  source.write_file("other.txt", "x");
  write_manifest("@/output.txt = run build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));
  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");
  EXPECT_EQ(runs, 1);

  source.write_file("other.txt", "y");  // not a dependency of output.txt
  EXPECT_EQ(runs, 1);                   // so no rebuild
}

// A never-read output is not eagerly built: with nothing having read it, it has
// no recorded dependencies to react to, and stays lazy until first read.
TEST_F(BuildDirectoryTreeTest, DoesNotEagerlyBuildAnUnreadOutput) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt = run build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));

  source.write_file("input.txt", "v2");
  EXPECT_EQ(runs, 0);  // never read, so never built

  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");  // built on first read
  EXPECT_EQ(runs, 1);
}

// A rebuild replaces the tracked dependency set: an input the command stops
// reading no longer triggers rebuilds, and one it starts reading now does.
TEST_F(BuildDirectoryTreeTest, ARebuildRefreshesTheTrackedInputs) {
  source.write_file("a.txt", "1");
  source.write_file("b.txt", "1");
  write_manifest("@/output.txt = run build %out\n");

  int runs = 0;
  // The first build reads a.txt; every rebuild thereafter reads b.txt instead.
  const BuildDirectoryTree tree(source, [&runs](BuildDirectoryTree::Command) {
    ++runs;
    std::vector<std::filesystem::path> inputs =
        runs == 1 ? std::vector<std::filesystem::path>{"a.txt"}
                  : std::vector<std::filesystem::path>{"b.txt"};
    return finished(BuildDirectoryTree::BuildOutput{
        .bytes = "build " + std::to_string(runs), .inputs = std::move(inputs)});
  });

  read_output(tree, "output.txt");  // runs=1, depends on a.txt
  EXPECT_EQ(runs, 1);

  source.write_file("a.txt", "2");  // rebuilds; now depends on b.txt
  EXPECT_EQ(runs, 2);

  source.write_file("a.txt", "3");  // no longer a dependency
  EXPECT_EQ(runs, 2);

  source.write_file("b.txt", "2");  // now it is
  EXPECT_EQ(runs, 3);
}

// An input change that lands while a rebuild is in flight does not start a
// second concurrent build; instead the in-flight one, on completing, notices it
// went stale and rebuilds once more - coalescing the changes.
TEST_F(BuildDirectoryTreeTest, CoalescesInputChangesDuringAnInFlightRebuild) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt = run build %out\n");

  // Build #1 completes at once; later ones are held in flight.
  const DeferredRunner deferred;
  int runs = 0;
  const BuildDirectoryTree tree(
      source,
      [&, held = deferred.runner()](BuildDirectoryTree::Command command) {
        ++runs;
        if (runs == 1) {
          return finished(BuildDirectoryTree::BuildOutput{
              .bytes = "build 1", .inputs = {"input.txt"}});
        }
        return held(std::move(command));
      });

  const auto complete = [&deferred](std::string bytes) {
    deferred.complete(BuildDirectoryTree::BuildOutput{.bytes = std::move(bytes),
                                                      .inputs = {"input.txt"}});
  };

  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(deferred.runs(), 0);

  // An input change starts eager rebuild #2 (in flight, not yet reported).
  source.write_file("input.txt", "v2");
  EXPECT_EQ(runs, 2);
  EXPECT_EQ(deferred.runs(), 1);

  // A second change while #2 is in flight starts no new build yet.
  source.write_file("input.txt", "v3");
  EXPECT_EQ(runs, 2);
  EXPECT_EQ(deferred.runs(), 1);

  // Completing #2 sees the output went stale mid-build and kicks off #3.
  complete("build 2");
  EXPECT_EQ(runs, 3);
  EXPECT_EQ(deferred.runs(), 2);

  // #3 settles it: no further change arrived, so no rebuild #4.
  complete("build 3");
  EXPECT_EQ(read_output(tree, "output.txt"), "build 3");
  EXPECT_EQ(runs, 3);
}

// Opening a dirty output waits for its rebuild; status() meanwhile reports the
// previous build's size.
TEST_F(BuildDirectoryTreeTest, OpeningADirtyOutputWaitsForItsRebuild) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt = run build %out\n");

  const DeferredRunner builds;
  const BuildDirectoryTree tree(source, builds.runner());
  const auto result = [](std::string bytes) {
    return BuildDirectoryTree::BuildOutput{.bytes = std::move(bytes),
                                           .inputs = {"input.txt"}};
  };

  std::thread first([&] { EXPECT_EQ(read_output(tree, "output.txt"), "v1"); });
  ASSERT_TRUE(builds.wait_for_runs(1));
  builds.complete(result("v1"));
  first.join();

  // Starts an eager rebuild, held in flight.
  source.write_file("input.txt", "v2 is longer");
  ASSERT_TRUE(builds.wait_for_runs(2));
  EXPECT_EQ(output_size(tree, "output.txt"), 2U);

  std::atomic<bool> returned = false;
  std::string content;
  std::thread reader([&] {
    content = read_output(tree, "output.txt");
    returned = true;
  });
  // Its rebuild is still pending.
  EXPECT_FALSE(returned);

  builds.complete(result("v2 is longer"));
  reader.join();
  EXPECT_EQ(content, "v2 is longer");
  EXPECT_EQ(builds.runs(), 2);
}

// ---------------------------------------------------------------------------
// Reloading the rules when build.makebelieve changes.
// ---------------------------------------------------------------------------

// A runner whose output names the command it ran, recording each command.
BuildDirectoryTree::CommandRunner echoing(std::vector<std::string>& commands) {
  return [&commands](BuildDirectoryTree::Command command) {
    commands.push_back(command.text);
    return finished(built("ran " + command.text));
  };
}

TEST_F(BuildDirectoryTreeTest, AManifestWrittenLaterAddsItsOutputs) {
  std::vector<std::string> commands;
  const BuildDirectoryTree tree(source, echoing(commands));
  EXPECT_TRUE(tree.ls("")->empty());

  write_manifest("@/out/file.txt = run first %out\n");

  EXPECT_EQ(output_size(tree, "out/file.txt"), 1U);  // unbuilt
  EXPECT_TRUE(commands.empty());
  EXPECT_EQ(read_output(tree, "out/file.txt"), "ran first %out");
}

TEST_F(BuildDirectoryTreeTest, AddingARuleLeavesTheOthersBuilt) {
  write_manifest("@/a.txt = run a %out\n");
  std::vector<std::string> commands;
  const BuildDirectoryTree tree(source, echoing(commands));
  EXPECT_EQ(read_output(tree, "a.txt"), "ran a %out");

  write_manifest("@/a.txt = run a %out\n@/b.txt = run b %out\n");

  EXPECT_EQ(output_size(tree, "a.txt"), 10U);  // kept its built content
  EXPECT_EQ(output_size(tree, "b.txt"), 1U);   // new and unbuilt
  EXPECT_EQ(commands.size(), 1U);
  EXPECT_EQ(read_output(tree, "b.txt"), "ran b %out");
}

TEST_F(BuildDirectoryTreeTest, RemovingARuleRemovesItsOutputAndEmptyParents) {
  write_manifest(
      "@/keep.txt = run keep %out\n@/gone/deep/file.txt = run gone %out\n");
  std::vector<std::string> commands;
  const BuildDirectoryTree tree(source, echoing(commands));
  EXPECT_EQ(read_output(tree, "gone/deep/file.txt"), "ran gone %out");

  write_manifest("@/keep.txt = run keep %out\n");

  EXPECT_FALSE(tree.status("gone/deep/file.txt").has_value());
  EXPECT_FALSE(tree.status("gone").has_value());
  EXPECT_EQ(tree.open("gone/deep/file.txt").error(),
            std::errc::no_such_file_or_directory);
  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  ASSERT_EQ(root->size(), 1U);
  EXPECT_EQ((*root)[0].name, "keep.txt");
}

TEST_F(BuildDirectoryTreeTest, AnOutputCanReplaceADirectoryOfRemovedOutputs) {
  write_manifest("@/out/file.txt = run nested %out\n");
  std::vector<std::string> commands;
  const BuildDirectoryTree tree(source, echoing(commands));

  write_manifest("@/out = run flat %out\n");

  EXPECT_EQ(read_output(tree, "out"), "ran flat %out");
}

TEST_F(BuildDirectoryTreeTest, DeletingTheManifestRemovesEveryOutput) {
  write_manifest("@/a.txt = run a %out\n@/dir/b.txt = run b %out\n");
  std::vector<std::string> commands;
  const BuildDirectoryTree tree(source, echoing(commands));

  source.remove("build.makebelieve");

  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  EXPECT_TRUE(root->empty());
}

// A changed command rebuilds an opened output eagerly, like a changed input,
// and leaves outputs whose rule did not change alone.
TEST_F(BuildDirectoryTreeTest, ChangingACommandRebuildsAnOpenedOutput) {
  write_manifest("@/a.txt = run a1 %out\n@/b.txt = run b %out\n");
  std::vector<std::string> commands;
  const BuildDirectoryTree tree(source, echoing(commands));
  EXPECT_EQ(read_output(tree, "a.txt"), "ran a1 %out");
  EXPECT_EQ(read_output(tree, "b.txt"), "ran b %out");

  write_manifest("@/a.txt = run a2 %out\n@/b.txt = run b %out\n");

  ASSERT_EQ(commands.size(), 3U);
  EXPECT_EQ(commands[2], "a2 %out");
  EXPECT_EQ(read_output(tree, "a.txt"), "ran a2 %out");
  EXPECT_EQ(commands.size(), 3U);
}

TEST_F(BuildDirectoryTreeTest, ChangingACommandLeavesAnUnopenedOutputLazy) {
  write_manifest("@/a.txt = run a1 %out\n");
  std::vector<std::string> commands;
  const BuildDirectoryTree tree(source, echoing(commands));

  write_manifest("@/a.txt = run a2 %out\n");

  EXPECT_TRUE(commands.empty());
  EXPECT_EQ(read_output(tree, "a.txt"), "ran a2 %out");
}

// A build that finishes after its rule was removed does not bring the output
// back, and the open waiting on it reports it missing.
TEST_F(BuildDirectoryTreeTest, ABuildFinishingAfterItsRuleIsRemovedIsDropped) {
  write_manifest("@/output.txt = run build %out\n");

  const DeferredRunner builds;
  const BuildDirectoryTree tree(source, builds.runner());

  std::expected<FileInfo, std::error_code> opened;
  std::thread reader([&] { opened = tree.open("output.txt"); });
  ASSERT_TRUE(builds.wait_for_runs(1));

  write_manifest("");
  EXPECT_FALSE(tree.status("output.txt").has_value());

  builds.complete(built("too late"));
  reader.join();
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(), std::errc::no_such_file_or_directory);
  EXPECT_FALSE(tree.status("output.txt").has_value());
  EXPECT_EQ(builds.runs(), 1);
}

TEST_F(BuildDirectoryTreeTest, ABadManifestAtStartupThrows) {
  write_manifest("@/ok.txt = run a %out\nnot a rule\n");
  std::vector<std::string> commands;
  try {
    const BuildDirectoryTree tree(source, echoing(commands));
    ADD_FAILURE() << "expected the manifest to be rejected";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(
        std::string_view(error.what()).starts_with("build.makebelieve:2: "))
        << error.what();
  }
}

// A bad edit - a typo, or rules that clash - is reported and ignored, leaving
// the previous rules and their built outputs in place until it is fixed.
TEST_F(BuildDirectoryTreeTest, ABadEditKeepsThePreviousRules) {
  write_manifest("@/a.txt = run a %out\n");
  std::vector<std::string> commands;
  std::vector<std::string> problems;
  const BuildDirectoryTree tree(
      source, echoing(commands),
      [&problems](const std::string& text) { problems.push_back(text); });
  EXPECT_EQ(read_output(tree, "a.txt"), "ran a %out");

  write_manifest("@/a.txt = run a %out\n@/b.txt run b %out\n");
  write_manifest("@/a.txt = run a %out\n@/a.txt/b = run b %out\n");

  ASSERT_EQ(problems.size(), 2U);
  EXPECT_TRUE(problems[0].starts_with("build.makebelieve:2: ")) << problems[0];
  EXPECT_TRUE(problems[1].starts_with("build.makebelieve:2: ")) << problems[1];
  EXPECT_EQ(output_size(tree, "a.txt"), 10U);  // still built
  EXPECT_FALSE(tree.status("b.txt").has_value());

  write_manifest("@/a.txt = run a %out\n@/b.txt = run b %out\n");
  EXPECT_EQ(problems.size(), 2U);
  EXPECT_EQ(output_size(tree, "a.txt"), 10U);
  EXPECT_EQ(read_output(tree, "b.txt"), "ran b %out");
}

// Forwards to an InMemoryDirectoryTree, but can be told to fail every read.
class FlakyTree : public DirectoryTree {
 public:
  InMemoryDirectoryTree inner;
  std::atomic<bool> fail_reads = false;

  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path) const override {
    return inner.status(path);
  }
  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path) const override {
    return inner.ls(path);
  }
  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size) const override {
    if (fail_reads) {
      return std::unexpected(
          std::make_error_code(std::errc::permission_denied));
    }
    return inner.read(path, offset, size);
  }
  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback)
      const override {
    return inner.subscribe_to_changes(callback);
  }
};

// A manifest that exists but cannot be read (say, locked by an editor) is not
// mistaken for an empty one.
TEST_F(BuildDirectoryTreeTest, AnUnreadableManifestKeepsThePreviousRules) {
  FlakyTree flaky;
  flaky.inner.write_file("build.makebelieve", "@/a.txt = run a %out\n");
  std::vector<std::string> commands;
  std::vector<std::string> problems;
  const BuildDirectoryTree tree(
      flaky, echoing(commands),
      [&problems](const std::string& text) { problems.push_back(text); });

  flaky.fail_reads = true;
  flaky.inner.write_file("build.makebelieve", "@/b.txt = run b %out\n");

  ASSERT_EQ(problems.size(), 1U);
  EXPECT_TRUE(problems[0].starts_with("build.makebelieve: ")) << problems[0];
  EXPECT_TRUE(tree.status("a.txt").has_value());

  flaky.fail_reads = false;
  flaky.inner.write_file("build.makebelieve", "@/b.txt = run b %out\n");
  EXPECT_FALSE(tree.status("a.txt").has_value());
  EXPECT_TRUE(tree.status("b.txt").has_value());
}

TEST_F(BuildDirectoryTreeTest, AnUnreadableManifestAtStartupThrows) {
  FlakyTree flaky;
  flaky.inner.write_file("build.makebelieve", "@/a.txt = run a %out\n");
  flaky.fail_reads = true;
  std::vector<std::string> commands;
  EXPECT_THROW(BuildDirectoryTree(flaky, echoing(commands)),
               std::runtime_error);
}

// Removing a rule forgets its traced inputs, so they no longer rebuild it once
// it is declared again.
TEST_F(BuildDirectoryTreeTest, ARemovedRuleForgetsItsDependencies) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt = run build %out\n");
  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));
  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");

  write_manifest("");
  write_manifest("@/output.txt = run build %out\n");
  source.write_file("input.txt", "v2");

  EXPECT_EQ(runs, 1);  // re-declared unbuilt, so nothing to rebuild eagerly
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);
}

// A `tracing` output is the installed tracer's trace, served by the tree
// without involving the runner, and it shows the builds that went before.
TEST_F(BuildDirectoryTreeTest, ATracingOutputServesTheTrace) {
  write_manifest(
      "@/trace.json = tracing\n"
      "@/output.txt = run build %out\n");

  Tracer tracer;
  const TracerInstallation installed(tracer);
  int runs = 0;
  const BuildDirectoryTree tree(source, counting_with_inputs(runs, {}));

  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");
  const std::string trace = read_output(tree, "trace.json");
  EXPECT_EQ(runs, 1);
  EXPECT_TRUE(trace.starts_with("[")) << trace;
  EXPECT_TRUE(trace.ends_with("]\n")) << trace;
  EXPECT_TRUE(trace.contains(R"("ph":"B","cat":"build","name":"output.txt")"))
      << trace;
  EXPECT_TRUE(trace.contains(R"("ph":"E","cat":"build","name":"output.txt")"))
      << trace;
  EXPECT_TRUE(trace.contains(R"("command":"build %out")")) << trace;
  // The build runs on a row of its own, with an arrow from whatever asked for
  // it.
  EXPECT_TRUE(trace.contains(R"("args":{"name":"build"})")) << trace;
  EXPECT_TRUE(trace.contains(R"("ph":"s","cat":"build","name":"build")"))
      << trace;
  EXPECT_TRUE(trace.contains(R"("ph":"f","cat":"build","name":"build")"))
      << trace;
  // Its own build is left out, as its snapshot could only ever show it begun.
  EXPECT_FALSE(trace.contains(R"("name":"trace.json")")) << trace;
}

// Every open takes a fresh snapshot, so the trace includes whatever happened
// since the last one.
TEST_F(BuildDirectoryTreeTest, ATracingOutputIsFreshOnEveryOpen) {
  write_manifest(
      "@/trace.json = tracing\n"
      "@/output.txt = run build %out\n");

  Tracer tracer;
  const TracerInstallation installed(tracer);
  int runs = 0;
  const BuildDirectoryTree tree(source, counting_with_inputs(runs, {}));

  const std::string before = read_output(tree, "trace.json");
  EXPECT_FALSE(before.contains(R"("name":"output.txt")")) << before;

  read_output(tree, "output.txt");
  const std::string after = read_output(tree, "trace.json");
  EXPECT_TRUE(after.contains(R"("name":"output.txt")")) << after;
  EXPECT_EQ(output_size(tree, "trace.json"), after.size());
}

TEST_F(BuildDirectoryTreeTest, ATracingOutputWithoutATracerIsAnEmptyTrace) {
  write_manifest("@/trace.json = tracing\n");

  ASSERT_EQ(g_tracer, nullptr);
  int runs = 0;
  const BuildDirectoryTree tree(source, counting_with_inputs(runs, {}));

  EXPECT_EQ(read_output(tree, "trace.json"), "[]\n");
  EXPECT_EQ(runs, 0);
}

// Builds share a pool of rows in the trace: one row each while they overlap,
// and a row goes back in the pool for the next build once its own span is
// closed.
TEST_F(BuildDirectoryTreeTest, BuildsShareRowsInTheTrace) {
  write_manifest(
      "@/a.txt = run build a %out\n"
      "@/b.txt = run build b %out\n"
      "@/c.txt = run build c %out\n");

  Tracer tracer;
  const TracerInstallation installed(tracer);
  const DeferredRunner runner;
  const BuildDirectoryTree tree(source, runner.runner());

  // Two builds at once, so they cannot share a row.
  std::thread first([&tree] { EXPECT_TRUE(tree.open("a.txt").has_value()); });
  std::thread second([&tree] { EXPECT_TRUE(tree.open("b.txt").has_value()); });
  ASSERT_TRUE(runner.wait_for_runs(2));
  runner.complete(built("one"));
  runner.complete(built("two"));
  first.join();
  second.join();

  // Both rows are free again, so a third build takes one rather than adding to
  // them.
  std::thread third([&tree] { EXPECT_TRUE(tree.open("c.txt").has_value()); });
  ASSERT_TRUE(runner.wait_for_runs(3));
  runner.complete(built("three"));
  third.join();

  // Two rows were needed for the builds that overlapped, and the third build
  // took one of them back rather than making another.
  const std::string trace = tracer.snapshot();
  std::size_t rows = 0;
  for (std::size_t at = trace.find(R"("args":{"name":"build"})");
       at != std::string::npos;
       at = trace.find(R"("args":{"name":"build"})", at + 1)) {
    ++rows;
  }
  EXPECT_EQ(rows, 2U) << trace;
}

// Reading the trace rewrites it; announcing that would have a watcher that
// rereads changed files reread the trace forever. Adding the output is still
// announced.
TEST_F(BuildDirectoryTreeTest, RewritingATracingOutputIsNotAnnounced) {
  write_manifest("@/output.txt = run build %out\n");

  Tracer tracer;
  const TracerInstallation installed(tracer);
  int runs = 0;
  const BuildDirectoryTree tree(source, counting_with_inputs(runs, {}));

  std::vector<std::filesystem::path> changed;
  const Subscription subscription =
      tree.subscribe_to_changes([&changed](const DirectoryTreeDiff& diff) {
        changed.insert(changed.end(), diff.entries_changed.begin(),
                       diff.entries_changed.end());
      });

  write_manifest(
      "@/output.txt = run build %out\n"
      "@/trace.json = tracing\n");
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0], std::filesystem::path("trace.json"));

  changed.clear();
  read_output(tree, "trace.json");
  read_output(tree, "trace.json");
  read_output(tree, "output.txt");
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0], std::filesystem::path("output.txt"));
}

#if defined(_WIN32) || defined(__linux__)
// End to end: a real watched source, real shell builds, and real tracing.
// Editing a traced input on disk rebuilds the output that read it, with no one
// re-reading it. Guarded to the platforms where tracing is wired up.
TEST_F(BuildDirectoryTreeTest, RebuildsThroughRealTracingWhenAnInputChanges) {
  using namespace std::chrono_literals;

  write_input("input.txt", "v1");
  // `cmake -E copy` reads input.txt (which the tracer sees) into %out.
  std::ofstream(work / "build.makebelieve", std::ios::binary)
      << "@/output.txt = run cmake -E copy input.txt %out\n";

  const RealDirectoryTree real_source(work, io);
  const BuildDirectoryTree tree(real_source, BuildDirectoryTree::shell_runner(
                                                 work, pool.get_scheduler()));

  // First read builds it and traces input.txt as a dependency.
  EXPECT_EQ(read_output(tree, "output.txt"), "v1");

  // Change the input on disk; the watcher notices and the output rebuilds. That
  // is asynchronous (it runs on the watcher thread), so poll for it.
  write_input("input.txt", "v2-changed");

  std::string content;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    std::this_thread::sleep_for(50ms);
    content = read_output(tree, "output.txt");
  } while (content != "v2-changed" &&
           std::chrono::steady_clock::now() < deadline);

  EXPECT_EQ(content, "v2-changed");
}

// End to end: a `copy` rule reports the file it names as its input, so the
// output rebuilds when that file changes - with no command tracing involved.
TEST_F(BuildDirectoryTreeTest, RebuildsACopyWhenItsSourceChanges) {
  using namespace std::chrono_literals;

  write_input("input.txt", "v1");
  std::ofstream(work / "build.makebelieve", std::ios::binary)
      << "@/output.txt = copy input.txt\n";

  const RealDirectoryTree real_source(work, io);
  const BuildDirectoryTree tree(real_source, BuildDirectoryTree::shell_runner(
                                                 work, pool.get_scheduler()));

  EXPECT_EQ(read_output(tree, "output.txt"), "v1");

  // A copy builds in microseconds, so this gets here well before the watcher
  // has finished arming and a single write could go unseen; keep rewriting the
  // input until the rebuild it triggers lands.
  std::string content;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    write_input("input.txt", "v2-changed");
    std::this_thread::sleep_for(50ms);
    content = read_output(tree, "output.txt");
  } while (content != "v2-changed" &&
           std::chrono::steady_clock::now() < deadline);

  EXPECT_EQ(content, "v2-changed");
}

// End to end: editing build.makebelieve on disk reloads the rules.
TEST_F(BuildDirectoryTreeTest, ReloadsTheManifestWhenItChangesOnDisk) {
  using namespace std::chrono_literals;

  std::ofstream(work / "build.makebelieve", std::ios::binary)
      << "@/old.txt = run cmake -E echo_append old > %out\n";

  const RealDirectoryTree real_source(work, io);
  const BuildDirectoryTree tree(real_source, BuildDirectoryTree::shell_runner(
                                                 work, pool.get_scheduler()));
  ASSERT_TRUE(tree.status("old.txt").has_value());

  // The watcher arms asynchronously, so a single early write can go unseen;
  // keep rewriting the manifest until the change is picked up.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while ((tree.status("old.txt").has_value() ||
          !tree.status("new.txt").has_value()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::ofstream(work / "build.makebelieve", std::ios::binary)
        << "@/new.txt = run cmake -E echo_append new > %out\n";
    std::this_thread::sleep_for(100ms);
  }

  EXPECT_FALSE(tree.status("old.txt").has_value());
  EXPECT_EQ(read_output(tree, "new.txt"), "new");
}
#endif

}  // namespace
