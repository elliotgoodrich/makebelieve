// SPDX-License-Identifier: MIT
#include "consoleinterrupthandler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <filesystem>
#include <iterator>
#endif

namespace {

TEST(ConsoleInterruptHandler, TokenIsArmedButUnsetBeforeInterrupt) {
  const makebelieve::ConsoleInterruptHandler handler;
  const std::stop_token token = handler.token();
  EXPECT_TRUE(token.stop_possible());
  EXPECT_FALSE(token.stop_requested());
}

TEST(ConsoleInterruptHandler, SecondConstructionThrows) {
  const makebelieve::ConsoleInterruptHandler handler;
  // Creating 2 handlers violated our preconditions, but we throw in
  // this case to make it easier to test.
  EXPECT_THROW(makebelieve::ConsoleInterruptHandler{}, std::logic_error);
}

TEST(ConsoleInterruptHandler, CanReinstallOnceTheFirstIsDestroyed) {
  { const makebelieve::ConsoleInterruptHandler first; }
  // If the destructor failed to release the single-instance slot, this second
  // construction would throw instead.
  EXPECT_NO_THROW(makebelieve::ConsoleInterruptHandler{});
}

TEST(ConsoleInterruptHandler, ConstructionSurfacesAnInstallFailure) {
  // Interpreted through std::system_category, so its message() is whatever the
  // OS generates for this code (for example "Access is denied." on Windows).
  constexpr int fake_error = 5;
  makebelieve::ConsoleInterruptHandlerTestUtil::fail_next_install(fake_error);

  try {
    const makebelieve::ConsoleInterruptHandler handler;
    ADD_FAILURE() << "construction should have thrown";
    // Only reached if the injection did not fire; clear it so it cannot leak
    // into a later test.
    makebelieve::ConsoleInterruptHandlerTestUtil::reset();
  } catch (const std::system_error& error) {
    EXPECT_EQ(error.code(),
              std::error_code(fake_error, std::system_category()));

    // Check the error code message is in the exception's what() string, so the user sees it.
    // Don't check for its contents directly, since that is OS-dependent and we don't want to
    // hard-code a string that will break on some platforms.
    const std::string os_text = error.code().message();
    ASSERT_FALSE(os_text.empty());

    const std::string message = error.what();
    EXPECT_NE(message.find(os_text), std::string::npos);
    EXPECT_NE(message.find("install the console interrupt handler"),
              std::string::npos);
  }

  // A failed construction must still release the single-instance slot, so a
  // fresh handler can be created afterwards. If the throwing path left g_source
  // set, this second construction would incorrectly throw std::logic_error.
  EXPECT_NO_THROW(makebelieve::ConsoleInterruptHandler{});
}

// ---------------------------------------------------------------------------
// The real interrupt path, end to end - one test per backend, since how an
// interrupt is delivered is exactly what differs.
// ---------------------------------------------------------------------------

#ifdef _WIN32

// A console control event can only be delivered to a process group sharing the
// console, so signalling this process would also hit the test runner. The
// subject therefore has to be a separate process in its own group; this test
// spawns one (a re-exec of this same binary with --serve) and fires CTRL_BREAK
// at just that group.

// Name of the event the --serve child sets once its handler is installed, so
// the parent never fires the interrupt into a window where the child would
// still get the OS default (terminate) instead of our handler.
constexpr wchar_t k_ready_event_name[] = L"makebelieve_cih_test_ready";

TEST(ConsoleInterruptHandler, InterruptRequestsStop) {
  wchar_t exe_path[MAX_PATH];
  ASSERT_NE(::GetModuleFileNameW(nullptr, exe_path, MAX_PATH), 0u);

  // Auto-reset: the child sets it once, the parent waits once.
  const HANDLE ready =
      ::CreateEventW(nullptr, FALSE, FALSE, k_ready_event_name);
  ASSERT_NE(ready, nullptr);

  std::wstring command = L"\"";
  command += exe_path;
  command += L"\" --serve";

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};

  // CREATE_NEW_PROCESS_GROUP puts the child in a group of its own, so the
  // CTRL_BREAK below reaches it and nothing else - this test process included.
  const BOOL created = ::CreateProcessW(
      nullptr, command.data(), nullptr, nullptr, TRUE,
      CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process);
  ASSERT_TRUE(created) << "CreateProcessW failed: " << ::GetLastError();

  // Wait for the child to install its handler before signalling. A timeout
  // here is a real failure - the child never got ready.
  ASSERT_EQ(::WaitForSingleObject(ready, 10000), WAIT_OBJECT_0)
      << "child did not become ready";

  ASSERT_TRUE(::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process.dwProcessId))
      << "GenerateConsoleCtrlEvent failed: " << ::GetLastError()
      << " (this test needs a real console)";

  // The child must return promptly once its token fires. A timeout here means
  // the interrupt did not reach the token, or wait-for-stop never woke.
  EXPECT_EQ(::WaitForSingleObject(process.hProcess, 10000), WAIT_OBJECT_0)
      << "child did not exit after the interrupt";

  DWORD exit_code = 1;
  EXPECT_TRUE(::GetExitCodeProcess(process.hProcess, &exit_code));
  EXPECT_EQ(exit_code, 0u) << "child exited uncleanly after the interrupt";

  ::CloseHandle(process.hProcess);
  ::CloseHandle(process.hThread);
  ::CloseHandle(ready);
}

// Runs in the child spawned by InterruptRequestsStop: install the handler,
// announce readiness, block until the token fires, and exit 0. Kept separate
// from wait_for_stop() in main.cpp - duplicating a handful of lines is cheaper
// than exporting that helper just for a test.
int serve() {
  const makebelieve::ConsoleInterruptHandler handler;
  const std::stop_token token = handler.token();

  if (const HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                        k_ready_event_name);
      ready != nullptr) {
    ::SetEvent(ready);
    ::CloseHandle(ready);
  }

  std::binary_semaphore stopped{0};
  const std::stop_callback callback{token, [&stopped] { stopped.release(); }};
  stopped.acquire();
  return 0;
}

#else  // _WIN32

// On POSIX there is no console-group complication: a signal can be raised on
// this very process, and our handler must turn it into a stop request rather
// than letting the default disposition terminate the runner. So the subject is
// this process itself - no child, no re-exec.
TEST(ConsoleInterruptHandler, InterruptRequestsStop) {
  const makebelieve::ConsoleInterruptHandler handler;
  const std::stop_token token = handler.token();
  ASSERT_TRUE(token.stop_possible());
  ASSERT_FALSE(token.stop_requested());

  // Arm the wait before raising, so a stop that fires at once is still caught.
  // The hand-off goes through the signal handler's self-pipe and the dispatcher
  // thread, so it is asynchronous - wait for the token rather than reading it
  // straight after raise() returns.
  std::binary_semaphore stopped{0};
  const std::stop_callback callback{token, [&stopped] { stopped.release(); }};

  ASSERT_EQ(::raise(SIGINT), 0);

  ASSERT_TRUE(stopped.try_acquire_for(std::chrono::seconds(10)))
      << "SIGINT never reached the token";
  EXPECT_TRUE(token.stop_requested());
}

// The process's open descriptors, or nothing where /proc is not mounted.
// directory_iterator holds one of its own while it runs, which is fine as long
// as only counts taken the same way are compared.
std::optional<int> count_open_fds() {
  std::error_code error;
  const std::filesystem::directory_iterator entries{"/proc/self/fd", error};
  if (error) {
    return std::nullopt;
  }
  return static_cast<int>(
      std::distance(std::filesystem::begin(entries),
                    std::filesystem::end(entries)));
}

// Unlike the Windows backend, this one has a self-pipe and a dispatcher thread
// standing before it ever asks the OS to install anything, so a refusal at the
// install step has real state to unwind. fail_next_install() is injected at
// exactly that point, which makes this the test for that rollback.
TEST(ConsoleInterruptHandler, AFailedInstallLeavesNothingBehind) {
  // The self-pipe is built on first use and then held for the life of the
  // process, so the baseline has to be taken with it already standing. Without
  // this the two descriptors would read as a leak whenever this test happens to
  // run first.
  { const makebelieve::ConsoleInterruptHandler warm_up; }

  const std::optional<int> before = count_open_fds();
  if (!before.has_value()) {
    GTEST_SKIP() << "/proc/self/fd is unavailable, cannot count descriptors";
  }

  // Repeated so that anything the rollback drops accumulates into an obvious
  // difference rather than a single descriptor that is easy to miss.
  constexpr int attempts = 50;
  for (int i = 0; i < attempts; ++i) {
    makebelieve::ConsoleInterruptHandlerTestUtil::fail_next_install(5);
    EXPECT_THROW(makebelieve::ConsoleInterruptHandler{}, std::system_error);
  }
  EXPECT_EQ(count_open_fds(), before) << "the failed installs leaked descriptors";

  // The rollback also has to put SIGINT back as it found it and leave no
  // dispatcher behind, so a handler built afterwards must still work end to
  // end rather than merely construct.
  const makebelieve::ConsoleInterruptHandler handler;
  std::binary_semaphore stopped{0};
  const std::stop_callback callback{handler.token(),
                                    [&stopped] { stopped.release(); }};
  ASSERT_EQ(::raise(SIGINT), 0);
  EXPECT_TRUE(stopped.try_acquire_for(std::chrono::seconds(10)))
      << "the handler stopped working after a failed install";
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // The Windows interrupt test re-execs this binary as the signal subject.
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--serve") {
      return serve();
    }
  }
#endif
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
