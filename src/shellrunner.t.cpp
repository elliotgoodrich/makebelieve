// SPDX-License-Identifier: MIT
#include "shellrunner.hpp"

#include "commandrunner.hpp"
#include "iocontext.hpp"
#include "manifest.hpp"

#include <gtest/gtest.h>
#include <exec/single_thread_context.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

namespace {

using namespace makebelieve;
using namespace std::chrono_literals;

namespace ex = stdexec;

// The path a command echoed back, without the newline (and, under cmd, the
// carriage return) that echoing appended.
std::filesystem::path trimmed_path(std::string_view echoed) {
  const std::size_t end = echoed.find_last_not_of("\r\n");
  return echoed.substr(0, end == std::string_view::npos ? 0 : end + 1);
}

// What a build completed with, and the thread it completed on.
struct Built {
  BuildResult result;
  std::thread::id completed_on;
};

template <class Scheduler>
std::thread::id thread_of(Scheduler scheduler) {
  return std::get<0>(ex::sync_wait(ex::schedule(scheduler) | ex::then([] {
                                     return std::this_thread::get_id();
                                   }))
                         .value());
}

class ShellRunnerTest : public ::testing::Test {
 protected:
  IoContext io;

  // One thread, so the thread a build ran on can be checked.
  exec::single_thread_context context;

  std::filesystem::path work =
      std::filesystem::temp_directory_path() /
      ("makebelieve-shell-" +
       std::string(
           ::testing::UnitTest::GetInstance()->current_test_info()->name()));

  // Stands in for the mount, holding the outputs a command reaches as `@/`.
  // With a space in it, so a path through it has to be quoted to survive.
  std::filesystem::path mountpoint = work.string() + " mnt";

  void SetUp() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::remove_all(mountpoint, ec);
    std::filesystem::create_directories(work);
    std::filesystem::create_directories(mountpoint / "sub");
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::remove_all(mountpoint, ec);
  }

  void write_generated(const std::filesystem::path& name,
                       std::string_view content) {
    std::ofstream(mountpoint / name, std::ios::binary) << content;
  }

  void write_input(std::string_view name, std::string_view content) {
    std::ofstream(work / name, std::ios::binary) << content;
  }

  // Runs @a command through @a run, stoppable through @a stop, registering
  // what it launches with @a registrar.
  template <class Scheduler>
  static Built build(const ShellRunner<Scheduler>& run,
                     Command command,
                     ex::inplace_stop_token stop = {},
                     LaunchRegistrar registrar = LaunchRegistrar::none()) {
    return std::get<0>(
        ex::sync_wait(
            ex::write_env(run(std::move(command)),
                          ex::env{ex::prop{ex::get_stop_token, stop},
                                  ex::prop{get_launch_registrar, registrar}}) |
            ex::then([](BuildResult result) {
              return Built{.result = std::move(result),
                           .completed_on = std::this_thread::get_id()};
            }))
            .value());
  }

  [[nodiscard]] Built build(
      Command command,
      ex::inplace_stop_token stop = {},
      LaunchRegistrar registrar = LaunchRegistrar::none()) {
    const ShellRunner run(work, mountpoint, io, context.get_scheduler());
    return build(run, std::move(command), stop, registrar);
  }
};

// A receiver whose environment has a stop token but no registrar - as one
// that some wrapper had dropped it from would.
struct ReceiverWithoutRegistrar {
  using receiver_concept = ex::receiver_t;
  void set_value(BuildResult) noexcept {}
  void set_error(std::exception_ptr) noexcept {}
  void set_stopped() noexcept {}
  [[nodiscard]] auto get_env() const noexcept {
    return ex::prop{ex::get_stop_token, ex::inplace_stop_token{}};
  }
};

// The same receiver with a registrar too.
struct ReceiverWithRegistrar : ReceiverWithoutRegistrar {
  [[nodiscard]] auto get_env() const noexcept {
    return ex::env{ex::prop{ex::get_stop_token, ex::inplace_stop_token{}},
                   ex::prop{get_launch_registrar, LaunchRegistrar::none()}};
  }
};

// A build cannot run anywhere its processes would go unregistered: losing the
// registrar on the way is a compile error, not a silent default.
static_assert(!ex::sender_to<BuildSender, ReceiverWithoutRegistrar>);
static_assert(ex::sender_to<BuildSender, ReceiverWithRegistrar>);

// A file outside the working directory, which on Linux a command sees only
// through the read-only tracing mount, for a command to create to show it ran.
std::filesystem::path marker_file() {
  return std::filesystem::temp_directory_path() /
         ("makebelieve-shell-ran-" +
          std::string(
              ::testing::UnitTest::GetInstance()->current_test_info()->name()));
}

// The registrar named in the receiver's environment reaches the command
// through the type-erased BuildSender and the hop onto the runner's scheduler,
// and is told about the command before the command does anything.
TEST_F(ShellRunnerTest, RegistersEachCommandBeforeItRuns) {
  const std::filesystem::path marker = marker_file();
  std::filesystem::remove(marker);
  int registered = 0;
  bool ran_first = false;
  bool undone = false;
  auto registrar = [&](const LaunchedProcess& process)
      -> std::expected<LaunchRegistration, std::error_code> {
    ++registered;
    ran_first = std::filesystem::exists(marker);
    EXPECT_NE(process.pid, 0U);
    return LaunchRegistration([&undone] { undone = true; });
  };
  const Built built =
      build({.output = "output.txt",
             .action = Manifest::Action::Capture,
             .text = "cmake -E touch \"" + marker.string() + "\""},
            {}, LaunchRegistrar(registrar));
  ASSERT_TRUE(built.result.has_value()) << built.result.error().message();
  EXPECT_TRUE(std::filesystem::exists(marker));
  std::filesystem::remove(marker);
  EXPECT_EQ(registered, 1);
  EXPECT_FALSE(ran_first);
  EXPECT_TRUE(undone);
}

TEST_F(ShellRunnerTest, ACommandThatCannotBeRegisteredNeverRunsAndFails) {
  const std::error_code refusal =
      std::make_error_code(std::errc::resource_unavailable_try_again);
  auto registrar = [&](const LaunchedProcess&)
      -> std::expected<LaunchRegistration, std::error_code> {
    return std::unexpected(refusal);
  };
  const std::filesystem::path marker = marker_file();
  std::filesystem::remove(marker);
  const Built built =
      build({.output = "output.txt",
             .action = Manifest::Action::Capture,
             .text = "cmake -E touch \"" + marker.string() + "\""},
            {}, LaunchRegistrar(registrar));
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error(), refusal);
  EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(ShellRunnerTest, CopiesTheFileACopyRuleNamesAndReportsItAsTheInput) {
  write_input("input.txt", "copied");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Copy,
                             .text = "input.txt"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.result->bytes, "copied");
  EXPECT_EQ(built.result->inputs,
            std::vector<std::filesystem::path>{"input.txt"});
}

TEST_F(ShellRunnerTest, ProducesWhatARunCommandWroteToOut) {
  write_input("input.txt", "written");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E copy input.txt %out"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.result->bytes, "written");
}

// A run command that leaves nothing at %out has not built its output, so it
// fails rather than producing an empty one.
TEST_F(ShellRunnerTest, FailsARunCommandThatWritesNothingToOut) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E echo nothing"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error(), std::errc::no_such_file_or_directory);
}

TEST_F(ShellRunnerTest, FailsACommandThatExitsNonzero) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "exit 2"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error(), makebelieve::exit_status_error(2));
  EXPECT_EQ(built.result.error().message(), "command exited with status 2");
}

TEST_F(ShellRunnerTest, ProducesWhatACaptureCommandWroteToStandardOutput) {
  write_input("input.txt", "captured");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat input.txt"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.result->bytes, "captured");
}

// The scratch file `%out` names has the file name of the output being built,
// so a tool that picks its format from the extension (pandoc and friends)
// needs no extra flag. `cmake -E echo` prints the path it was handed under
// either shell, redirected into that same file so it becomes the output.
TEST_F(ShellRunnerTest, NamesTheScratchFileAfterTheOutput) {
  const Built built =
      build({.output = std::filesystem::path("docs") / "report.pdf",
             .action = Manifest::Action::Run,
             .text = "cmake -E echo %out > %out"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(trimmed_path(built.result->bytes).filename(), "report.pdf");
}

// An output with no extension leaves the scratch file without one either,
// rather than inventing something for a tool to sniff.
TEST_F(ShellRunnerTest, AddsNoExtensionToABareOutput) {
  const Built built = build({.output = "report",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E echo %out > %out"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(trimmed_path(built.result->bytes).filename(), "report");
}

// `@/` in a command names another output, reached through the mountpoint -
// quoted, since the mountpoint may hold spaces.
TEST_F(ShellRunnerTest, ExpandsAnOutputInACaptureCommandToItsPathInTheMount) {
  write_generated(std::filesystem::path("sub") / "generated.txt", "generated");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat @/sub/generated.txt"});
  ASSERT_TRUE(built.result.has_value()) << built.result.error().message();
  EXPECT_EQ(built.result->bytes, "generated");
}

TEST_F(ShellRunnerTest, ExpandsAnOutputInARunCommandAlongsideOut) {
  write_generated("generated.txt", "generated");
  write_input("input.txt", "input");
  const Built built =
      build({.output = "output.txt",
             .action = Manifest::Action::Run,
             .text = "cmake -E cat input.txt @/generated.txt > %out"});
  ASSERT_TRUE(built.result.has_value()) << built.result.error().message();
  EXPECT_EQ(built.result->bytes, "inputgenerated");
}

// A `@/` path already inside quotes runs to the closing quote - spaces and
// all - and is left to those quotes rather than quoted again.
TEST_F(ShellRunnerTest, ExpandsAnOutputAlreadyInsideQuotes) {
  write_generated("my file.txt", "spaced");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat \"@/my file.txt\""});
  ASSERT_TRUE(built.result.has_value()) << built.result.error().message();
  EXPECT_EQ(built.result->bytes, "spaced");
}

// A command reading an output through the mount builds on whatever it finds
// there; one that is missing fails the command like any missing file.
TEST_F(ShellRunnerTest, FailsACommandReadingAMissingOutput) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat @/missing.txt"});
  EXPECT_FALSE(built.result.has_value());
}

// A copy takes the bytes verbatim, so a binary source survives intact.
TEST_F(ShellRunnerTest, CopiesBytesVerbatim) {
  const std::string binary("a\0b\r\nc", 6);
  write_input("input.bin", binary);
  const Built built = build({.output = "output.bin",
                             .action = Manifest::Action::Copy,
                             .text = "input.bin"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.result->bytes, binary);
}

// A copy whose source is not there fails, with the reason, rather than
// quietly producing nothing.
TEST_F(ShellRunnerTest, FailsACopyOfAMissingFile) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Copy,
                             .text = "missing.txt"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error(), std::errc::no_such_file_or_directory);
}

// The execution requirement: a build's synchronous steps run on the given
// scheduler, and it completes there too, whether or not it ran a command -
// never on the thread that started it.
TEST_F(ShellRunnerTest, RunsAndCompletesEachBuildOnItsScheduler) {
  write_input("input.txt", "text");
  const std::thread::id scheduler_thread = thread_of(context.get_scheduler());
  ASSERT_NE(scheduler_thread, std::this_thread::get_id());

  for (const Command& command :
       {Command{.output = "copy.txt",
                .action = Manifest::Action::Copy,
                .text = "input.txt"},
        Command{.output = "run.txt",
                .action = Manifest::Action::Run,
                .text = "cmake -E copy input.txt %out"},
        Command{.output = "capture.txt",
                .action = Manifest::Action::Capture,
                .text = "cmake -E cat input.txt"}}) {
    const Built built = build(command);
    ASSERT_TRUE(built.result.has_value()) << command.text;
    EXPECT_EQ(built.completed_on, scheduler_thread) << command.text;
  }
}

// Any scheduler meeting the requirements will do: here a thread pool's, as
// the daemon uses.
TEST_F(ShellRunnerTest, RunsOnAThreadPool) {
  write_input("input.txt", "pooled");
  exec::static_thread_pool pool(1);
  const ShellRunner run(work, mountpoint, io, pool.get_scheduler());
  const Built built = build(run, {.output = "output.txt",
                                  .action = Manifest::Action::Capture,
                                  .text = "cmake -E cat input.txt"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.result->bytes, "pooled");
  EXPECT_EQ(built.completed_on, thread_of(pool.get_scheduler()));
}

TEST_F(ShellRunnerTest, StoppingABuildEndsItsCommand) {
  ex::inplace_stop_source stop;
  std::jthread stopper([&stop] {
    std::this_thread::sleep_for(200ms);
    stop.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E sleep 20"},
                            stop.get_token());
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error(), std::errc::operation_canceled);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
}

}  // namespace
