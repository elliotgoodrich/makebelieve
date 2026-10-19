// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include "buildprocesstracker.hpp"
#include "iocontext.hpp"

#include <gtest/gtest.h>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstddef>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/stat.h>
#endif

#ifdef __linux__
#include <sys/resource.h>
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
    makebelieve::ProcessUtilTestUtil::reset();
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
  }

  void write_input(std::string_view name, std::string_view content) {
    std::ofstream stream(work / name, std::ios::binary);
    stream << content;
  }

  makebelieve::IoContext io;

  // For the runs that no build owns.
  makebelieve::BuildProcessTracker& none =
      makebelieve::BuildProcessTracker::none();

  // Runs @a command in @a working_directory and returns what it completes
  // with.
  makebelieve::ProcessUtil::Result run_in(
      const std::filesystem::path& working_directory,
      const std::string& command,
      stdexec::inplace_stop_token stop = {}) {
    return std::get<0>(
        stdexec::sync_wait(makebelieve::ProcessUtil::run(
                               io, working_directory, command, none, {}, stop))
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

// A file outside the working directory, which on Linux a command sees only
// through the read-only tracing mount, for a command to create to show it ran.
std::filesystem::path marker_file() {
  return std::filesystem::temp_directory_path() /
         ("makebelieve-proc-ran-" +
          std::string(
              ::testing::UnitTest::GetInstance()->current_test_info()->name()));
}

// A build process tracker that records each process added to it, checking that
// the command had not yet run - it creates @a marker - when it was added. It
// finds nothing.
struct RecordingBuildProcessTracker final : makebelieve::BuildProcessTracker {
  std::filesystem::path marker;
  int added = 0;
  bool ran_before_adding = false;
  bool undone = false;
  std::filesystem::path output;
  makebelieve::ProcessHandle process;

  explicit RecordingBuildProcessTracker(std::filesystem::path created)
      : marker(std::move(created)) {}

  std::expected<Token, std::error_code> add(
      const std::filesystem::path& built,
      const makebelieve::ProcessHandle& launched) override {
    ++added;
    ran_before_adding = std::filesystem::exists(marker);
    output = built;
    process = launched;
    return Token{1};
  }

  void remove(Token /*token*/) noexcept override { undone = true; }

  std::optional<std::filesystem::path> find(
      std::uint32_t /*requester_pid*/) override {
    return std::nullopt;
  }
};

// A build process tracker that refuses to add anything, with @a refusal.
struct RefusingBuildProcessTracker final : makebelieve::BuildProcessTracker {
  std::error_code refusal;

  explicit RefusingBuildProcessTracker(std::error_code error)
      : refusal(error) {}

  std::expected<Token, std::error_code> add(
      const std::filesystem::path& /*output*/,
      const makebelieve::ProcessHandle& /*process*/) override {
    return std::unexpected(refusal);
  }

  void remove(Token /*token*/) noexcept override {}

  std::optional<std::filesystem::path> find(
      std::uint32_t /*requester_pid*/) override {
    return std::nullopt;
  }
};

TEST_F(ProcessUtil, AddsTheCommandBeforeItRunsAndRemovesItAfter) {
  const std::filesystem::path marker = marker_file();
  std::filesystem::remove(marker);
  RecordingBuildProcessTracker recording(marker);
  const std::filesystem::path k_output = "out/built.txt";
  const makebelieve::ProcessUtil::Result result = std::get<0>(
      stdexec::sync_wait(makebelieve::ProcessUtil::run(
                             io, work,
                             "cmake -E touch \"" + marker.string() + "\"",
                             recording, k_output, {}))
          .value());
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->exit_status, 0);
  EXPECT_TRUE(std::filesystem::exists(marker));
  std::filesystem::remove(marker);
  EXPECT_EQ(recording.added, 1);
  EXPECT_FALSE(recording.ran_before_adding);
  EXPECT_EQ(recording.output, k_output);
  EXPECT_NE(recording.process.pid, 0U);
#ifdef _WIN32
  EXPECT_NE(recording.process.job, nullptr);
#else
  EXPECT_GE(recording.process.pidfd, 0);
#endif
  EXPECT_TRUE(recording.undone);
}

TEST_F(ProcessUtil, ACommandThatCannotBeAddedNeverRuns) {
  const std::error_code refusal =
      std::make_error_code(std::errc::permission_denied);
  RefusingBuildProcessTracker refusing(refusal);
  const std::filesystem::path marker = marker_file();
  std::filesystem::remove(marker);
  const makebelieve::ProcessUtil::Result result = std::get<0>(
      stdexec::sync_wait(makebelieve::ProcessUtil::run(
                             io, work,
                             "cmake -E touch \"" + marker.string() + "\"",
                             refusing, "out/built.txt", {}))
          .value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), refusal);
  EXPECT_FALSE(std::filesystem::exists(marker));
}

// A build process tracker that calls @a on_add with each process added to it,
// before letting it run. It finds nothing.
struct CallingBuildProcessTracker final : makebelieve::BuildProcessTracker {
  std::function<void(const makebelieve::ProcessHandle&)> on_add;

  explicit CallingBuildProcessTracker(
      std::function<void(const makebelieve::ProcessHandle&)> call)
      : on_add(std::move(call)) {}

  std::expected<Token, std::error_code> add(
      const std::filesystem::path& /*output*/,
      const makebelieve::ProcessHandle& process) override {
    on_add(process);
    return Token{1};
  }

  void remove(Token /*token*/) noexcept override {}

  std::optional<std::filesystem::path> find(
      std::uint32_t /*requester_pid*/) override {
    return std::nullopt;
  }
};

// A command launched while another is being launched inherits none of the
// other's descriptors - which would hold its pipe, and on Linux its tracing
// mount, open for as long as the second command lived. The second command is
// launched from the tracker while the first is added to it, so with every
// descriptor the first has; what it inherited must match what it inherits
// launched alone.
TEST_F(ProcessUtil, ACommandInheritsNothingFromAnotherBeingLaunched) {
#ifdef _WIN32
  // Counted while it is suspended, before any code of its own has run, every
  // handle it holds was inherited or made by the launch.
  const std::string command = "cmake -E true";
#else
  // Its shell, and the init it runs under in its own pid namespace.
  const std::string command = "ls /proc/1/fd /proc/$$/fd";
#endif
  // What the second command inherited.
  const auto run_second = [&] {
    std::string inherited;
    CallingBuildProcessTracker tracker(
        [&](const makebelieve::ProcessHandle& process) {
#ifdef _WIN32
          const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, process.pid);
          DWORD count = 0;
          if (handle != nullptr && GetProcessHandleCount(handle, &count)) {
            inherited = std::to_string(count);
          }
          if (handle != nullptr) {
            CloseHandle(handle);
          }
#else
          static_cast<void>(process);
#endif
        });
    const makebelieve::ProcessUtil::Result result = std::get<0>(
        stdexec::sync_wait(makebelieve::ProcessUtil::run(io, work, command,
                                                         tracker, "second", {}))
            .value());
    EXPECT_TRUE(result.has_value()) << result.error().message();
#ifndef _WIN32
    if (result.has_value()) {
      inherited = result->standard_output;
    }
#endif
    return inherited;
  };

  const std::string alone = run_second();
  ASSERT_FALSE(alone.empty());

  std::string during_launch;
  CallingBuildProcessTracker first(
      [&](const makebelieve::ProcessHandle& /*process*/) {
        during_launch = run_second();
      });
  const makebelieve::ProcessUtil::Result result =
      std::get<0>(stdexec::sync_wait(makebelieve::ProcessUtil::run(
                                         io, work, command, first, "first", {}))
                      .value());
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(during_launch, alone);
}

// Waiting on a command holds no thread: ten one-second commands started from
// one thread finish together rather than one after another.
TEST_F(ProcessUtil, RunsCommandsAtOnceWithoutAThreadEach) {
  const auto start = std::chrono::steady_clock::now();
  const auto results = stdexec::sync_wait(stdexec::when_all(
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {}),
      makebelieve::ProcessUtil::run(io, work, sleep_command(1), none, {}, {})));
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

TEST_F(ProcessUtil, ReportsTheStatusACommandExitedWith) {
  write_input("input.txt", "hello world");

  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt") + " && exit 3");

  // A nonzero status is the caller's to judge; what the command did is still
  // reported in full.
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->exit_status, 3);
  EXPECT_EQ(result->standard_output, "hello world");
  EXPECT_FALSE(result->inputs.empty());
}

// Makes our stdin, while it lives, a pipe that never has anything to read nor
// closes - so anything reading it waits forever, as on a terminal.
class StdinThatNeverEnds {
#ifdef _WIN32
  HANDLE m_saved = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE m_read = nullptr;
  HANDLE m_write = nullptr;

 public:
  StdinThatNeverEnds() {
    SECURITY_ATTRIBUTES inheritable = {
        .nLength = sizeof(inheritable),
        .bInheritHandle = TRUE,
    };
    if (CreatePipe(&m_read, &m_write, &inheritable, 0)) {
      SetStdHandle(STD_INPUT_HANDLE, m_read);
    }
  }

  ~StdinThatNeverEnds() {
    SetStdHandle(STD_INPUT_HANDLE, m_saved);
    if (m_read != nullptr) {
      CloseHandle(m_read);
      CloseHandle(m_write);
    }
  }

  [[nodiscard]] bool valid() const { return m_read != nullptr; }
#else
  int m_saved = ::dup(STDIN_FILENO);
  std::array<int, 2> m_pipe{-1, -1};

 public:
  StdinThatNeverEnds() {
    if (m_saved >= 0 && ::pipe2(m_pipe.data(), O_CLOEXEC) == 0) {
      ::dup2(m_pipe[0], STDIN_FILENO);
    }
  }

  ~StdinThatNeverEnds() {
    if (m_saved >= 0) {
      ::dup2(m_saved, STDIN_FILENO);
      ::close(m_saved);
    }
    for (const int fd : m_pipe) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }

  [[nodiscard]] bool valid() const { return m_pipe[0] >= 0; }
#endif

  StdinThatNeverEnds(const StdinThatNeverEnds&) = delete;
  StdinThatNeverEnds& operator=(const StdinThatNeverEnds&) = delete;
  StdinThatNeverEnds(StdinThatNeverEnds&&) = delete;
  StdinThatNeverEnds& operator=(StdinThatNeverEnds&&) = delete;
};

// A command reads no input, rather than ours: from a terminal, a read would
// block it on the console or - in its own process group - stop it for good,
// and the build with it. Given a stdin that never ends, one reading to the end
// of its input still finishes at once, having read nothing.
TEST_F(ProcessUtil, ACommandReadsNoInput) {
  const StdinThatNeverEnds stdin_that_never_ends;
  ASSERT_TRUE(stdin_that_never_ends.valid());

  // Stopped if it waits, so a failure fails rather than hangs.
  stdexec::inplace_stop_source source;
  std::jthread stopper([&source](const std::stop_token& done) {
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any().wait_for(lock, done, 10s,
                                           [] { return false; });
    source.request_stop();
  });
#ifdef _WIN32
  const std::string command = R"(%SystemRoot%\System32\sort.exe)";
#else
  const std::string command = "cat";
#endif
  const makebelieve::ProcessUtil::Result result =
      run(command, source.get_token());

  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->exit_status, 0);
  EXPECT_EQ(result->standard_output, "");
}

#ifdef __linux__
// The tracer lets the kernel cache what a command reads, so a file rewritten
// while the command runs - by an earlier build step finishing, say - must read
// as its new contents when the command next opens it, not as the cached old.
TEST_F(ProcessUtil, ReadsAFileRewrittenBetweenOpensAsItsNewContents) {
  write_input("input.txt", "before");

  // FIFOs outside the working directory, which the command sees untraced:
  // through "opened" it says it has read the file once, and through "rewritten"
  // it waits until the file has changed.
  const std::filesystem::path opened = marker_file().string() + "-opened";
  const std::filesystem::path rewritten = marker_file().string() + "-rewritten";
  std::error_code ec;
  std::filesystem::remove(opened, ec);
  std::filesystem::remove(rewritten, ec);
  ASSERT_EQ(::mkfifo(opened.c_str(), 0600), 0);
  ASSERT_EQ(::mkfifo(rewritten.c_str(), 0600), 0);

  std::jthread rewriter([&] {
    std::string line;
    std::getline(std::ifstream(opened), line);
    // The same size, so that only the contents differ.
    write_input("input.txt", "after!");
    std::ofstream(rewritten) << "\n";
  });

  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt") + " && echo > \"" + opened.string() +
          "\" && read line < \"" + rewritten.string() + "\" && " +
          print_file_command("input.txt"));
  rewriter.join();
  std::filesystem::remove(opened, ec);
  std::filesystem::remove(rewritten, ec);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->standard_output, "beforeafter!");
}

// Compilers and linkers read their inputs through shared read-only mmaps, as
// this command does, so the tracer must allow them.
TEST_F(ProcessUtil, ReadsAFileTheCommandMapsShared) {
  write_input("input.txt", "hello world");

  // -I keeps python from listing the working directory for modules.
  const makebelieve::ProcessUtil::Result result =
      run("python3 -I -c \"import mmap, sys; f = open('input.txt', 'rb'); "
          "sys.stdout.buffer.write(mmap.mmap(f.fileno(), 0, mmap.MAP_SHARED, "
          "mmap.PROT_READ)[:])\"");

  ASSERT_TRUE(result.has_value());
  if (result->exit_status == 127) {
    GTEST_SKIP() << "python3 is not installed";
  }
  EXPECT_EQ(result->exit_status, 0);
  EXPECT_EQ(result->standard_output, "hello world");
}

TEST_F(ProcessUtil, ReportsACommandKilledByASignalAsTheShellWould) {
  const makebelieve::ProcessUtil::Result result = run("kill -KILL $$");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->exit_status, 128 + 9);
}

// The tracer holds no fd per file the command looks up, so a command reading
// more files than the process may have open at once still reads, and is traced
// reading, every one.
TEST_F(ProcessUtil, TracesMoreFilesThanTheProcessMayHaveOpen) {
  constexpr std::size_t k_files = 600;
  std::string command = "cat";
  for (std::size_t i = 0; i < k_files; ++i) {
    const std::string name = "input" + std::to_string(i) + ".txt";
    write_input(name, "x");
    command += " " + name;
  }

  rlimit original{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
  rlimit lowered = original;
  lowered.rlim_cur = 256;
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered), 0);
  const makebelieve::ProcessUtil::Result result = run(command);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &original), 0);

  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->exit_status, 0);
  EXPECT_EQ(result->standard_output, std::string(k_files, 'x'));
  EXPECT_EQ(result->inputs.size(), k_files);
}
#endif

// --- When a command cannot be traced ---------------------------------------
//
// Tracing is required, so each of these fails the run - and must leave nothing
// behind: no command still running, no callback outliving what it uses (which
// the sanitizer builds would catch), and nothing to stop the next run working.

using Failure = makebelieve::ProcessUtilTestUtil::Failure;

// Where a command leaves a marker if it ever runs: outside the working
// directory, which under tracing on Linux is a read-only view.
std::filesystem::path marker_path() {
  return std::filesystem::temp_directory_path() /
         ("makebelieve-marker-" +
          std::string(
              ::testing::UnitTest::GetInstance()->current_test_info()->name()));
}

std::string touch_command(const std::filesystem::path& path) {
  return "cmake -E touch \"" + path.string() + "\"";
}

TEST_F(ProcessUtil, ACommandThatCannotBeTracedNeverRuns) {
  const std::filesystem::path marker = marker_path();
  std::error_code ec;
  std::filesystem::remove(marker, ec);
  makebelieve::ProcessUtilTestUtil::fail_next(
      Failure::tracing_setup,
      std::make_error_code(std::errc::permission_denied));

  const makebelieve::ProcessUtil::Result result = run(touch_command(marker));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::permission_denied);
  EXPECT_FALSE(std::filesystem::exists(marker));

  // Nothing is left behind to trouble the next run.
  EXPECT_TRUE(run(touch_command(marker)).has_value());
  EXPECT_TRUE(std::filesystem::exists(marker));
  std::filesystem::remove(marker, ec);
}

#ifdef __linux__
// Servicing the mount failing loses reads, so the build fails - and the
// command, which would otherwise block on its unserved mount for ever, is
// ended rather than waited out.
TEST_F(ProcessUtil, ATracingFailureWhileRunningFailsTheRunAndEndsTheCommand) {
  write_input("input.txt", "hello world");
  makebelieve::ProcessUtilTestUtil::fail_next(
      Failure::tracing_service, std::make_error_code(std::errc::io_error));

  const auto start = std::chrono::steady_clock::now();
  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt") + " && " + sleep_command(20));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::io_error);
  EXPECT_LT(elapsed, 10s);

  EXPECT_TRUE(run(print_file_command("input.txt")).has_value());
}

// --- What the command leaves running ----------------------------------------

// Whatever the command started is ended with it - even a process that left
// its process group and session, as a daemon does - so it cannot go on to
// change files after the run.
TEST_F(ProcessUtil, EndsWhatTheCommandStartedEvenIfItDetached) {
  const std::filesystem::path marker = marker_path();
  std::error_code ec;
  std::filesystem::remove(marker, ec);

  const makebelieve::ProcessUtil::Result result =
      run("setsid sh -c 'sleep 1; " + touch_command(marker) +
          "' > /dev/null 2>&1 &");

  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->exit_status, 0);
  std::this_thread::sleep_for(3s);
  EXPECT_FALSE(std::filesystem::exists(marker));
  std::filesystem::remove(marker, ec);
}

// The run completes only once whatever the command started has exited. Here a
// child it leaves running holds a FIFO open for writing, which is closed only
// as the child exits: by the time the run completes, no writer is left.
TEST_F(ProcessUtil, CompletesOnlyOnceWhatTheCommandStartedHasExited) {
  const std::filesystem::path fifo = marker_path();
  std::error_code ec;
  std::filesystem::remove(fifo, ec);
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  // Opened before the command, so that its writers do not block opening it.
  const int reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
  ASSERT_GE(reader, 0);

  const auto start = std::chrono::steady_clock::now();
  const makebelieve::ProcessUtil::Result result =
      run("{ echo started; " + sleep_command(30) + " & } > \"" + fifo.string() +
          "\"");
  const auto elapsed = std::chrono::steady_clock::now() - start;

  std::string written;
  std::array<char, 64> buffer{};
  ssize_t count = 0;
  while ((count = ::read(reader, buffer.data(), buffer.size())) > 0) {
    written.append(buffer.data(), static_cast<std::size_t>(count));
  }
  const int read_error = errno;
  ::close(reader);
  std::filesystem::remove(fifo, ec);

  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(written, "started\n");
  // End of file, not EAGAIN: the backgrounded child has gone.
  EXPECT_EQ(count, 0) << std::strerror(read_error);
  // Ended rather than waited out.
  EXPECT_LT(elapsed, 10s);
}
#endif

#ifdef _WIN32
// A trace log that cannot be read back is an error, never an empty list of
// dependencies.
TEST_F(ProcessUtil, ATraceLogThatCannotBeReadFailsTheRun) {
  write_input("input.txt", "hello world");
  makebelieve::ProcessUtilTestUtil::fail_next(Failure::trace_log);

  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt"));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::no_such_file_or_directory);

  EXPECT_TRUE(run(print_file_command("input.txt")).has_value());
}

// A read the hook cannot append to the log is a lost dependency, so the run
// fails rather than succeeding with fewer inputs. Here the command itself
// locks the log against writers while it reads.
TEST_F(ProcessUtil, AReadThatCannotBeLoggedFailsTheRun) {
  write_input("input.txt", "hello world");

  const makebelieve::ProcessUtil::Result result = run(
      R"cmd(powershell.exe -NoProfile -NonInteractive -Command "$lock = [IO.File]::Open($env:MAKEBELIEVE_TRACE_LOG, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read); try { [Console]::Write([IO.File]::ReadAllText((Join-Path (Get-Location) 'input.txt'))) } finally { $lock.Dispose() }")cmd");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::io_error);

  EXPECT_TRUE(run(print_file_command("input.txt")).has_value());
}

// Tracing is mandatory, so a process whose hook cannot install never runs its
// own code - and the run fails.
TEST_F(ProcessUtil, ACommandWhoseHookCannotInstallNeverRuns) {
  const std::filesystem::path marker = marker_path();
  std::error_code ec;
  std::filesystem::remove(marker, ec);
  makebelieve::ProcessUtilTestUtil::fail_next(Failure::trace_hook_install);

  const makebelieve::ProcessUtil::Result result = run(touch_command(marker));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::io_error);
  EXPECT_FALSE(std::filesystem::exists(marker));

  EXPECT_TRUE(run(touch_command(marker)).has_value());
  std::filesystem::remove(marker, ec);
}

// An empty log from a command no hook ever ran in says nothing about what it
// read, so the run fails rather than reporting no inputs.
TEST_F(ProcessUtil, AHookThatNeverRunsFailsTheRun) {
  write_input("input.txt", "hello world");
  makebelieve::ProcessUtilTestUtil::fail_next(Failure::trace_hook_absent);

  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt"));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::operation_not_supported);

  EXPECT_TRUE(run(print_file_command("input.txt")).has_value());
}

// Every process is accounted for, not just some: a child that runs untraced
// fails the run even though the command itself was traced.
TEST_F(ProcessUtil, AChildThatRunsUntracedFailsTheRun) {
  write_input("input.txt", "hello world");
  makebelieve::ProcessUtilTestUtil::fail_next(
      Failure::trace_hook_absent_in_children);

  // cmd.exe, which is traced, starts cmake, which is not.
  const makebelieve::ProcessUtil::Result result =
      run(print_file_command("input.txt"));

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::operation_not_supported);

  EXPECT_TRUE(run(print_file_command("input.txt")).has_value());
}

// A child handed a tracing variable of its own, in a case other than ours, is
// traced under this run's value rather than its own.
TEST_F(ProcessUtil, TracesAChildGivenATracingVariableInAnotherCase) {
  write_input("input.txt", "hello world");

  const makebelieve::ProcessUtil::Result result = run(
      R"cmd(powershell.exe -NoProfile -NonInteractive -Command "$s = New-Object System.Diagnostics.ProcessStartInfo; $s.FileName = 'cmake.exe'; $s.Arguments = '-E cat input.txt'; $s.WorkingDirectory = (Get-Location).Path; $s.UseShellExecute = $false; $s.RedirectStandardOutput = $true; $s.EnvironmentVariables.Remove('MAKEBELIEVE_TRACE_ROOT'); $s.EnvironmentVariables['makebelieve_trace_root'] = 'C:\unrelated-root'; $p = [Diagnostics.Process]::Start($s); [Console]::Write($p.StandardOutput.ReadToEnd()); $p.WaitForExit(); exit $p.ExitCode")cmd");

  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->standard_output, "hello world");
  EXPECT_TRUE(std::ranges::any_of(result->inputs,
                                  [](const std::filesystem::path& path) {
                                    return path.filename() == "input.txt";
                                  }));
}

// Once a read cannot be recorded the command has lost a dependency, so it is
// ended then rather than left to run on.
TEST_F(ProcessUtil, AReadThatCannotBeLoggedEndsTheCommand) {
  write_input("input.txt", "hello world");

  const auto start = std::chrono::steady_clock::now();
  const makebelieve::ProcessUtil::Result result = run(
      R"cmd(powershell.exe -NoProfile -NonInteractive -Command "$lock = [IO.File]::Open($env:MAKEBELIEVE_TRACE_LOG, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read); try { [IO.File]::ReadAllText((Join-Path (Get-Location) 'input.txt')) | Out-Null; Start-Sleep -Seconds 20 } finally { $lock.Dispose() }")cmd");
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::io_error);
  EXPECT_LT(elapsed, 10s);
}

// A child started with an environment of its own - here, one without any of
// the tracing variables - is still traced.
TEST_F(ProcessUtil, TracesAChildGivenAnEnvironmentWithoutTracing) {
  write_input("input.txt", "hello world");

  const makebelieve::ProcessUtil::Result result = run(
      R"cmd(powershell.exe -NoProfile -NonInteractive -Command "$s = New-Object System.Diagnostics.ProcessStartInfo; $s.FileName = 'cmake.exe'; $s.Arguments = '-E cat input.txt'; $s.WorkingDirectory = (Get-Location).Path; $s.UseShellExecute = $false; $s.RedirectStandardOutput = $true; $s.EnvironmentVariables.Remove('MAKEBELIEVE_TRACE_STATUS'); $s.EnvironmentVariables.Remove('MAKEBELIEVE_TRACE_LOG'); $s.EnvironmentVariables.Remove('MAKEBELIEVE_TRACE_ROOT'); $p = [Diagnostics.Process]::Start($s); [Console]::Write($p.StandardOutput.ReadToEnd()); $p.WaitForExit(); exit $p.ExitCode")cmd");

  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->standard_output, "hello world");
  EXPECT_TRUE(std::ranges::any_of(result->inputs,
                                  [](const std::filesystem::path& path) {
                                    return path.filename() == "input.txt";
                                  }));
}
#endif

}  // namespace
