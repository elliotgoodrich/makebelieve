// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

namespace makebelieve {

/// @struct TraceStatus
/// Windows: how the tracing hook reports on itself to the run that injected
/// it. Each run creates one in named shared memory and names it in the
/// command's environment, as `k_status_var`; every process the hook is
/// injected into - the command and everything it starts - opens it.
///
/// Each process is accounted for: the run expects the command, each process
/// the hook sees started expects its child before starting it, and each hook
/// that installs acknowledges its own process. The run trusts its trace log
/// only if every process expected acknowledged and none reported a failure: a
/// process that ran without the hook, or whose hook could not record a read,
/// has lost a dependency.
struct TraceStatus {
  /// The environment variable naming the run's log, which the hook appends
  /// each read to.
  static constexpr wchar_t k_log_var[] = L"MAKEBELIEVE_TRACE_LOG";

  /// The environment variable naming the root whose reads are recorded.
  static constexpr wchar_t k_root_var[] = L"MAKEBELIEVE_TRACE_ROOT";

  /// The environment variable naming the shared memory holding this.
  static constexpr wchar_t k_status_var[] = L"MAKEBELIEVE_TRACE_STATUS";

  /// Appended to that name, the name of a manual-reset event a hook signals
  /// as it reports a failure, so the run can end the command at once rather
  /// than let it run on untraced.
  static constexpr wchar_t k_failure_event_suffix[] = L"-failed";

  /// Test-only: the environment variable telling the hook to fail as
  /// `k_fail_install` or `k_fail_absent` say.
  static constexpr wchar_t k_test_failure_var[] =
      L"MAKEBELIEVE_TRACE_TEST_FAILURE";

  /// Test-only: every hook reports that it could not install.
  static constexpr wchar_t k_fail_install[] = L"install";

  /// Test-only: every hook does nothing at all, as though never injected.
  static constexpr wchar_t k_fail_absent[] = L"absent";

  /// Test-only: the command's hook works, but it passes `k_fail_absent` on to
  /// everything the command starts.
  static constexpr wchar_t k_fail_absent_in_children[] = L"absent-in-children";

  /// How many processes are expected to install the hook.
  std::uint32_t expected;

  /// How many processes installed the hook.
  std::uint32_t installed;

  /// Nonzero once any process failed to install the hook or to record a read.
  /// Never cleared.
  std::uint32_t failed;

  /// Counts a process about to be started, before it can acknowledge.
  void expect_process() noexcept {
    std::atomic_ref(expected).fetch_add(1, std::memory_order_relaxed);
  }

  /// Uncounts a process expect_process() counted that could not be started.
  void unexpect_process() noexcept {
    std::atomic_ref(expected).fetch_sub(1, std::memory_order_relaxed);
  }

  void report_installed() noexcept {
    std::atomic_ref(installed).fetch_add(1, std::memory_order_relaxed);
  }

  void report_failure() noexcept {
    std::atomic_ref(failed).store(1, std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint32_t expected_processes() noexcept {
    return std::atomic_ref(expected).load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint32_t installs() noexcept {
    return std::atomic_ref(installed).load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool any_failed() noexcept {
    return std::atomic_ref(failed).load(std::memory_order_relaxed) != 0;
  }
};

static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free,
              "shared between processes, so it must not take a lock");

}  // namespace makebelieve
