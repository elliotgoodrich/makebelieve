// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include "iocontext.hpp"

#include <stdexec/execution.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#ifdef _WIN32
#include <array>

#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;

// `cmake -E` is a cross-platform command shim, so these commands run the same
// under whichever shell ProcessUtil drives; cmake is on PATH wherever ctest is.

// A command that prints the file @a name (resolved against the working
// directory) to stdout.
std::string print_file_command(std::string_view name) {
  return "cmake -E cat " + std::string(name);
}

// A command that blocks for about @a seconds, so cancellation has something to
// interrupt.
std::string sleep_command(int seconds) {
  return "cmake -E sleep " + std::to_string(seconds);
}

class ProcessUtil : public ::testing::Test {
 protected:
  std::filesystem::path work =
      std::filesystem::temp_directory_path() /
      ("makebelieve-proc-" +
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
    std::ofstream stream(work / name, std::ios::binary);
    stream << content;
  }

  makebelieve::IoContext io;

  // Runs @a command in @a working_directory and returns what it completes
  // with.
  makebelieve::ProcessUtil::Result run_in(
      const std::filesystem::path& working_directory,
      const std::string& command,
      stdexec::inplace_stop_token stop = {}) {
    return std::get<0>(
        stdexec::sync_wait(
            makebelieve::ProcessUtil::run(io, working_directory, command, stop))
            .value());
  }

  // Runs @a command in the working directory and returns what it completes
  // with.
  makebelieve::ProcessUtil::Result run(const std::string& command,
                                       stdexec::inplace_stop_token stop = {}) {
    return run_in(work, command, stop);
  }
};

TEST_F(ProcessUtil, CapturesStdoutOfACommandRunInTheWorkingDirectory) {
  write_input("input.txt", "hello world");

  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt"));

  ASSERT_TRUE(result.has_value());
  // `type` and `cat` copy the file's bytes to stdout verbatim.
  EXPECT_EQ(result->standard_output, "hello world");
}

TEST_F(ProcessUtil, TracesFilesTheCommandReadAsInputs) {
  write_input("input.txt", "hello world");

  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt"));

  ASSERT_TRUE(result.has_value());
#if defined(_WIN32) || defined(__linux__)
  // The command read input.txt; the tracer should report it, by absolute path
  // under the working directory. (Files it read outside the root - cmake's own
  // install, say - are filtered out, so this stays about the command's inputs.)
  const bool found =
      std::ranges::any_of(result->inputs, [&](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::equivalent(p, work / "input.txt", ec);
      });
  EXPECT_TRUE(found) << "expected input.txt among the traced reads";
#else
  // Tracing is not wired up on this platform yet, so inputs is always empty.
  EXPECT_TRUE(result->inputs.empty());
#endif
}

#ifdef _WIN32
// A working directory given in 8.3 short-name form - as a runner's temp path
// can be - must still trace reads under it. The hook resolves each opened file
// to its long canonical form, so the root it filters against is canonicalized
// to match; without that, nothing under a short-named root is reported.
TEST_F(ProcessUtil, TracesInputsWhenWorkingDirectoryIsAShortPath) {
  write_input("input.txt", "hello world");

  std::array<wchar_t, 32768> buffer{};
  const DWORD n = GetShortPathNameW(work.c_str(), buffer.data(),
                                    static_cast<DWORD>(buffer.size()));
  const std::filesystem::path short_work =
      (n > 0 && n < buffer.size()) ? std::filesystem::path(buffer.data())
                                   : work;
  if (short_work == work) {
    GTEST_SKIP() << "8.3 short names unavailable on this volume";
  }

  const makebelieve::ProcessUtil::Result result =
      run_in(short_work, print_file_command("input.txt"));

  ASSERT_TRUE(result.has_value());
  const bool found =
      std::ranges::any_of(result->inputs, [&](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::equivalent(p, work / "input.txt", ec);
      });
  EXPECT_TRUE(found) << "expected input.txt traced under a short-path root";
}
#endif

#ifdef __linux__
// A working directory reached through a symlink must still trace reads under
// it: the tracer canonicalizes the root, so a symlinked root and the reads
// resolve to the same path.
TEST_F(ProcessUtil, TracesInputsWhenWorkingDirectoryIsSymlinked) {
  write_input("input.txt", "hello world");

  const std::filesystem::path link =
      std::filesystem::temp_directory_path() /
      ("makebelieve-proc-link-" +
       std::string(
           ::testing::UnitTest::GetInstance()->current_test_info()->name()));
  std::error_code ec;
  std::filesystem::remove(link, ec);
  std::filesystem::create_directory_symlink(work, link, ec);
  if (ec) {
    GTEST_SKIP() << "could not create a directory symlink on this platform";
  }

  const makebelieve::ProcessUtil::Result result =
      run_in(link, print_file_command("input.txt"));
  std::filesystem::remove(link, ec);

  ASSERT_TRUE(result.has_value());
  const bool found =
      std::ranges::any_of(result->inputs, [&](const std::filesystem::path& p) {
        std::error_code equivalent_ec;
        return std::filesystem::equivalent(p, work / "input.txt",
                                           equivalent_ec);
      });
  EXPECT_TRUE(found) << "expected input.txt traced under a symlinked root";
}
#endif

// Waiting on a command holds no thread: ten one-second commands started from
// one thread finish together rather than one after another.
TEST_F(ProcessUtil, RunsCommandsAtOnceWithoutAThreadEach) {
  const auto start = std::chrono::steady_clock::now();
  const auto results = stdexec::sync_wait(stdexec::when_all(
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1)),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1))));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(results.has_value());
  const int succeeded = std::apply(
      [](const auto&... result) {
        return (static_cast<int>(result.has_value()) + ...);
      },
      *results);
  EXPECT_EQ(succeeded, 10);
  EXPECT_LT(elapsed, 6s);
}

TEST_F(ProcessUtil, ReportsCancellationWhenStopIsAlreadyRequested) {
  stdexec::inplace_stop_source source;
  source.request_stop();

  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt"), source.get_token());

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(),
            std::make_error_code(std::errc::operation_canceled));
}

TEST_F(ProcessUtil, TerminatesARunningCommandWhenStopIsRequested) {
  stdexec::inplace_stop_source source;
  std::jthread stopper([&source] {
    std::this_thread::sleep_for(200ms);
    source.request_stop();
  });

  const auto start = std::chrono::steady_clock::now();
  const makebelieve::ProcessUtil::Result result =
      run(sleep_command(20), source.get_token());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(),
            std::make_error_code(std::errc::operation_canceled));
  // Returned promptly on cancellation rather than waiting out the full sleep.
  EXPECT_LT(elapsed, 10s);
}

}  // namespace
