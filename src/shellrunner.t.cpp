// SPDX-License-Identifier: MIT
#include "shellrunner.hpp"

#include "buildprocesstracker.hpp"
#include "commandrunner.hpp"
#include "filesystemutil.hpp"
#include "iocontext.hpp"
#include "manifest.hpp"

#include <gtest/gtest.h>
#include <exec/single_thread_context.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
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
#include <vector>

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

  // What the file it built holds, read back - and the file deleted, as the
  // tree would once it took it over.
  std::string bytes;
};

// Reads the file @a path, then deletes it.
std::string take_file(const std::filesystem::path& path) {
  std::string bytes = FileSystemUtil::read_file(path).value_or("");
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return bytes;
}

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

  // Where the runner builds, as a cache's scratch directory would be.
  std::filesystem::path scratch = work.string() + " scratch";

  void SetUp() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::remove_all(mountpoint, ec);
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(work);
    std::filesystem::create_directories(mountpoint / "sub");
    std::filesystem::create_directories(scratch);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::remove_all(mountpoint, ec);
    std::filesystem::remove_all(scratch, ec);
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
                        Built built{.result = std::move(result),
                                    .completed_on = std::this_thread::get_id(),
                                    .bytes = {}};
                        if (built.result.has_value()) {
                          built.bytes = take_file(built.result->file);
                        }
                        return built;
                      }))
            .value());
  }

  // As above, through a runner that adds what it launches to @a tracker.
  [[nodiscard]] Built build(
      Command command,
      ex::inplace_stop_token stop = {},
      BuildProcessTracker& tracker = BuildProcessTracker::none()) {
    const ShellRunner run(work, mountpoint, scratch, io, tracker,
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
  ASSERT_TRUE(built.result.has_value()) << built.result.error().error.message();
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
  EXPECT_EQ(built.result.error().error, refusal);
  EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(ShellRunnerTest, CopiesTheFileACopyRuleNamesAndReportsItAsTheInput) {
  write_input("input.txt", "copied");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Copy,
                             .text = "input.txt"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.bytes, "copied");
  EXPECT_EQ(built.result->inputs,
            std::vector<std::filesystem::path>{"input.txt"});
}

// Every action builds into a file it leaves in the scratch directory, for the
// tree to take over, and leaves nothing else there - whatever else the
// command wrote beside %out included.
TEST_F(ShellRunnerTest, LeavesWhatItBuiltInTheScratchDirectoryAndNothingElse) {
  write_input("input.txt", "kept");
  for (const Command& command : {
           Command{.output = "sub/output.txt",
                   .action = Manifest::Action::Run,
                   .text = "cmake -E copy input.txt %out && cmake -E touch "
                           "%out.stray"},
           Command{.output = "output.txt",
                   .action = Manifest::Action::Capture,
                   .text = "cmake -E cat input.txt"},
           Command{.output = "output.txt",
                   .action = Manifest::Action::Copy,
                   .text = "input.txt"},
       }) {
    const ShellRunner run(work, mountpoint, scratch, io,
                          BuildProcessTracker::none(), context.get_scheduler());
    BuildResult result = std::get<0>(ex::sync_wait(run(command)).value());
    ASSERT_TRUE(result.has_value()) << command.text;
    EXPECT_EQ(result->file.parent_path(), scratch) << command.text;
    std::vector<std::filesystem::path> left;
    for (const auto& entry : std::filesystem::directory_iterator(scratch)) {
      left.push_back(entry.path());
    }
    EXPECT_EQ(left, std::vector{result->file}) << command.text;
    EXPECT_EQ(take_file(result->file), "kept") << command.text;
  }
}

TEST_F(ShellRunnerTest, ProducesWhatARunCommandWroteToOut) {
  write_input("input.txt", "written");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E copy input.txt %out"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.bytes, "written");
}

// A run command that leaves nothing at %out has not built its output, so it
// fails rather than producing an empty one - and not with the scratch file's
// own error, which would read as the output not existing.
TEST_F(ShellRunnerTest, FailsARunCommandThatWritesNothingToOut) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E echo nothing"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error().error,
            makebelieve::make_error_code(BuildError::out_not_written));
  EXPECT_NE(built.result.error().error, std::errc::no_such_file_or_directory);
  EXPECT_EQ(built.result.error().error.message(), "command did not write %out");
}

// The failure explains itself, and carries what the command printed, which is
// often the clue.
TEST_F(ShellRunnerTest, ReportsARunCommandThatWritesNothingToOut) {
  const Built built = build({.output = "sub/output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E echo forgot to write it"});
  ASSERT_FALSE(built.result.has_value());
  const std::string& errors = built.result.error().standard_error;
  EXPECT_TRUE(errors.starts_with("forgot to write it")) << errors;
  EXPECT_TRUE(errors.ends_with(
      "command exited successfully but wrote nothing to %out\n"))
      << errors;
}

// A run command's standard output is not its output, but may say why it
// failed, so it is kept with a failure's standard error rather than dropped.
TEST_F(ShellRunnerTest, KeepsWhatAFailingRunCommandPrints) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E echo progress && exit 1"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_TRUE(built.result.error().standard_error.starts_with("progress"))
      << built.result.error().standard_error;
}

#if defined(_WIN32) || defined(__linux__)
// A failing command still read its inputs, and they come back with the
// failure, so changing one - the fix, say - retries it.
TEST_F(ShellRunnerTest, KeepsWhatAFailingCommandRead) {
  write_input("input.txt", "read");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E copy input.txt %out && exit 3"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error().error, makebelieve::exit_status_error(3));
  EXPECT_TRUE(std::ranges::contains(built.result.error().inputs,
                                    std::filesystem::path("input.txt")));
}

// As does one that exits successfully without writing %out.
TEST_F(ShellRunnerTest, KeepsWhatACommandThatWritesNothingToOutRead) {
  write_input("input.txt", "read");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E cat input.txt"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error().error,
            makebelieve::make_error_code(BuildError::out_not_written));
  EXPECT_TRUE(std::ranges::contains(built.result.error().inputs,
                                    std::filesystem::path("input.txt")));
}
#endif

TEST_F(ShellRunnerTest, FailsACommandThatExitsNonzero) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "exit 2"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error().error, makebelieve::exit_status_error(2));
  EXPECT_EQ(built.result.error().error.message(),
            "command exited with status 2");
}

// What a failing command wrote to standard error comes back with the failure,
// for whoever reads the output to be told why.
TEST_F(ShellRunnerTest, KeepsWhatAFailingCommandWroteToStandardError) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E echo oops 1>&2 && exit 3"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error().error, makebelieve::exit_status_error(3));
  EXPECT_TRUE(built.result.error().standard_error.starts_with("oops"))
      << built.result.error().standard_error;
}

// Only the start of what a failing command wrote is kept - and nothing
// unbounded is held while it runs - with a line saying how much more there
// was.
TEST_F(ShellRunnerTest, KeepsOnlyTheStartOfWhatAFailingCommandWrote) {
  constexpr std::size_t k_written = 100000;
  write_input("big.txt", std::string(k_written, 'x'));
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat big.txt 1>&2 && exit 1"});
  ASSERT_FALSE(built.result.has_value());
  const std::string& kept = built.result.error().standard_error;
  EXPECT_EQ(kept,
            std::string(makebelieve::k_kept_diagnostics, 'x') + "\n... and " +
                std::to_string(k_written - makebelieve::k_kept_diagnostics) +
                " more bytes\n");
}

// A command that succeeds may still have warned, but it built, so that is not
// mixed into the output it built.
TEST_F(ShellRunnerTest, KeepsWhatASucceedingCommandWarnedOutOfItsOutput) {
  const Built built = build(
      {.output = "output.txt",
       .action = Manifest::Action::Capture,
       .text = "cmake -E echo warned 1>&2 && cmake -E echo_append built"});
  ASSERT_TRUE(built.result.has_value()) << built.result.error().error.message();
  EXPECT_EQ(built.bytes, "built");
}

TEST_F(ShellRunnerTest, ProducesWhatACaptureCommandWroteToStandardOutput) {
  write_input("input.txt", "captured");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat input.txt"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.bytes, "captured");
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
  EXPECT_EQ(trimmed_path(built.bytes).filename(), "report.pdf");
}

// An output with no extension leaves the scratch file without one either,
// rather than inventing something for a tool to sniff.
TEST_F(ShellRunnerTest, AddsNoExtensionToABareOutput) {
  const Built built = build({.output = "report",
                             .action = Manifest::Action::Run,
                             .text = "cmake -E echo %out > %out"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(trimmed_path(built.bytes).filename(), "report");
}

// `@/` in a command names another output, reached through the mountpoint -
// quoted, since the mountpoint may hold spaces.
TEST_F(ShellRunnerTest, ExpandsAnOutputInACaptureCommandToItsPathInTheMount) {
  write_generated(std::filesystem::path("sub") / "generated.txt", "generated");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat @/sub/generated.txt"});
  ASSERT_TRUE(built.result.has_value()) << built.result.error().error.message();
  EXPECT_EQ(built.bytes, "generated");
}

TEST_F(ShellRunnerTest, ExpandsAnOutputInARunCommandAlongsideOut) {
  write_generated("generated.txt", "generated");
  write_input("input.txt", "input");
  const Built built =
      build({.output = "output.txt",
             .action = Manifest::Action::Run,
             .text = "cmake -E cat input.txt @/generated.txt > %out"});
  ASSERT_TRUE(built.result.has_value()) << built.result.error().error.message();
  EXPECT_EQ(built.bytes, "inputgenerated");
}

// A `@/` path already inside quotes runs to the closing quote - spaces and
// all - and is left to those quotes rather than quoted again.
TEST_F(ShellRunnerTest, ExpandsAnOutputAlreadyInsideQuotes) {
  write_generated("my file.txt", "spaced");
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Capture,
                             .text = "cmake -E cat \"@/my file.txt\""});
  ASSERT_TRUE(built.result.has_value()) << built.result.error().error.message();
  EXPECT_EQ(built.bytes, "spaced");
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
        << directory << ": " << built.result.error().error.message();
    EXPECT_EQ(built.bytes, "generated") << directory;
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
  ASSERT_TRUE(built.result.has_value()) << built.result.error().error.message();
  EXPECT_EQ(built.bytes, "spaced");
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
  EXPECT_EQ(built.bytes, binary);
}

// A copy whose source is not there fails rather than quietly producing
// nothing - not with the source's own error, which would read as the output
// not existing, but saying why alongside.
TEST_F(ShellRunnerTest, FailsACopyOfAMissingFile) {
  const Built built = build({.output = "output.txt",
                             .action = Manifest::Action::Copy,
                             .text = "missing.txt"});
  ASSERT_FALSE(built.result.has_value());
  EXPECT_EQ(built.result.error().error,
            makebelieve::make_error_code(BuildError::copy_source_unreadable));
  EXPECT_NE(built.result.error().error, std::errc::no_such_file_or_directory);
  EXPECT_TRUE(built.result.error().standard_error.starts_with(
      "cannot copy [missing.txt]: "))
      << built.result.error().standard_error;
  // Nothing opened it, so no tracer could say; its input is what it names,
  // so creating it retries the copy.
  EXPECT_EQ(built.result.error().inputs,
            std::vector<std::filesystem::path>{"missing.txt"});
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
  const ShellRunner run(work, mountpoint, scratch, io,
                        BuildProcessTracker::none(), pool.get_scheduler());
  const Built built = build(run, {.output = "output.txt",
                                  .action = Manifest::Action::Capture,
                                  .text = "cmake -E cat input.txt"});
  ASSERT_TRUE(built.result.has_value());
  EXPECT_EQ(built.bytes, "pooled");
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
  EXPECT_EQ(built.result.error().error, std::errc::operation_canceled);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
}

}  // namespace
