// SPDX-License-Identifier: MIT
#include "shellrunner.hpp"

#include "buildprocesstracker.hpp"
#include "commandrunner.hpp"
#include "iocontext.hpp"
#include "manifest.hpp"

#include <gtest/gtest.h>
#include <exec/single_thread_context.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
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

  // As above, through a runner that adds what it launches to @a tracker.
  [[nodiscard]] Built build(
      Command command,
      ex::inplace_stop_token stop = {},
      BuildProcessTracker& tracker = BuildProcessTracker::none()) {
    const ShellRunner run(work, mountpoint, io, tracker,
                          context.get_scheduler());
    return build(run, std::move(command), stop);
  }
};

// A file outside the working directory, which on Linux a command sees only
// through the read-only tracing mount, for a command to create to show it ran.
std::filesystem::path marker_file() {
  return std::filesystem::temp_directory_path() /
         ("makebelieve-shell-ran-" +
          std::string(
              ::testing::UnitTest::GetInstance()->current_test_info()->name()));
}

// The runner adds each command to its tracker, before the command does
// anything, under the output the command builds - the path the build
// directory tree names that build by - however deep it is.
TEST_F(ShellRunnerTest, AddsEachCommandBeforeItRuns) {
  const std::filesystem::path marker = marker_file();
  std::filesystem::remove(marker);
  // Records each process added, and whether the command had already run.
  struct Recording final : BuildProcessTracker {
    std::filesystem::path marker;
    int added = 0;
    bool ran_first = false;
    bool undone = false;
    std::filesystem::path output;

    explicit Recording(std::filesystem::path created)
        : marker(std::move(created)) {}

    std::expected<Token, std::error_code> add(
        const std::filesystem::path& built,
        const ProcessHandle& process) override {
      ++added;
      ran_first = std::filesystem::exists(marker);
      output = built;
      EXPECT_NE(process.pid, 0U);
      return Token{1};
    }

    void remove(Token /*token*/) noexcept override { undone = true; }

    std::optional<std::filesystem::path> find(
        std::uint32_t /*requester_pid*/) override {
      return std::nullopt;
    }
  } recording(marker);
  const std::filesystem::path k_output = "out/sub/report.txt";
  const Built built =
      build({.output = k_output,
             .action = Manifest::Action::Capture,
             .text = "cmake -E touch \"" + marker.string() + "\""},
            {}, recording);
  ASSERT_TRUE(built.result.has_value()) << built.result.error().message();
  EXPECT_TRUE(std::filesystem::exists(marker));
  std::filesystem::remove(marker);
  EXPECT_EQ(recording.added, 1);
  EXPECT_FALSE(recording.ran_first);
  EXPECT_EQ(recording.output, k_output);
  EXPECT_TRUE(recording.undone);
}

TEST_F(ShellRunnerTest, ACommandThatCannotBeAddedNeverRunsAndFails) {
  const std::error_code refusal =
      std::make_error_code(std::errc::resource_unavailable_try_again);
  // Refuses to add anything.
  struct Refusing final : BuildProcessTracker {
    std::error_code refusal;

    explicit Refusing(std::error_code error) : refusal(error) {}

    std::expected<Token, std::error_code> add(
        const std::filesystem::path& /*output*/,
        const ProcessHandle& /*process*/) override {
      return std::unexpected(refusal);
    }

    void remove(Token /*token*/) noexcept override {}

    std::optional<std::filesystem::path> find(
        std::uint32_t /*requester_pid*/) override {
      return std::nullopt;
    }
  } refusing(refusal);
  const std::filesystem::path marker = marker_file();
  std::filesystem::remove(marker);
  const Built built =
      build({.output = "output.txt",
             .action = Manifest::Action::Capture,
             .text = "cmake -E touch \"" + marker.string() + "\""},
            {}, refusing);
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

// Writes, in @a directory, a program that copies the file its first argument
// names to its second, and returns its path.
std::filesystem::path write_copy_program(
    const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
#ifdef _WIN32
  const std::filesystem::path program = directory / "copy.cmd";
  std::ofstream(program) << "@copy /b %1 %2 > nul\n";
#else
  const std::filesystem::path program = directory / "copy";
  std::ofstream(program) << "#!/bin/sh\ncp \"$1\" \"$2\"\n";
  std::filesystem::permissions(program, std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::add);
#endif
  return program;
}

// A program named by a quoted path - whether or not it needs the quotes -
// runs alongside the quoted paths `@/` and `%out` expand to, which cmd would
// otherwise mangle by stripping the first and last quote on the line.
TEST_F(ShellRunnerTest, RunsAProgramNamedByAQuotedPath) {
  write_generated("generated.txt", "generated");
  for (const std::string_view directory : {"tools", "my tools"}) {
    const std::filesystem::path program = write_copy_program(work / directory);
    const Built built =
        build({.output = "output.txt",
               .action = Manifest::Action::Run,
               .text = "\"" + program.string() + "\" @/generated.txt %out"});
    ASSERT_TRUE(built.result.has_value())
        << directory << ": " << built.result.error().message();
    EXPECT_EQ(built.result->bytes, "generated") << directory;
  }
}

// Quoted arguments after an unquoted program keep working too.
TEST_F(ShellRunnerTest, RunsAProgramWithQuotedArguments) {
  write_generated("my file.txt", "spaced");
  const std::filesystem::path program = write_copy_program(work / "tools");
  const std::filesystem::path relative =
      std::filesystem::relative(program, work);
  const Built built =
      build({.output = "output.txt",
             .action = Manifest::Action::Run,
             .text = relative.string() + " \"@/my file.txt\" %out"});
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
  const ShellRunner run(work, mountpoint, io, BuildProcessTracker::none(),
                        pool.get_scheduler());
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
