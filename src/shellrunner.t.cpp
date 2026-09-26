// SPDX-License-Identifier: MIT
#include "shellrunner.hpp"

#include "commandrunner.hpp"
#include "iocontext.hpp"
#include "manifest.hpp"

#include <exec/single_thread_context.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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

  void SetUp() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
  }

  void write_input(std::string_view name, std::string_view content) {
    std::ofstream(work / name, std::ios::binary) << content;
  }

  // Runs @a command through @a run, stoppable through @a stop.
  template <class Scheduler>
  static Built build(const ShellRunner<Scheduler>& run,
                     Command command,
                     ex::inplace_stop_token stop = {}) {
    return std::get<0>(
        ex::sync_wait(ex::write_env(run(std::move(command)),
                                    ex::prop{ex::get_stop_token, stop}) |
                      ex::then([](BuildResult result) {
                        return Built{
                            .result = std::move(result),
                            .completed_on = std::this_thread::get_id()};
                      }))
            .value());
  }

  [[nodiscard]] Built build(Command command, ex::inplace_stop_token stop = {}) {
    const ShellRunner run(work, io, context.get_scheduler());
    return build(run, std::move(command), stop);
  }
};

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
  const ShellRunner run(work, io, pool.get_scheduler());
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
