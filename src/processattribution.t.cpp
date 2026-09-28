// SPDX-License-Identifier: MIT
#include "processattribution.hpp"

#include "iocontext.hpp"
#include "launchregistrar.hpp"
#include "processinfo.hpp"
#include "processutil.hpp"

#include <stdexec/execution.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace {

using namespace makebelieve;
using namespace std::chrono_literals;

constexpr AttemptId k_attempt = 42;

// Registers each process it is told about as k_attempt's, remembering its pid.
struct Registering {
  ProcessAttribution* attribution;
  std::atomic<std::uint32_t> pid = 0;

  std::expected<LaunchRegistration, std::error_code> operator()(
      const LaunchedProcess& process) {
    pid = process.pid;
    return attribution->register_process(k_attempt, process);
  }
};

TEST(ProcessAttribution, ResolvesACommandToItsAttemptOnlyWhileItIsRegistered) {
  IoContext io;
  ProcessAttribution attribution;
  Registering registering{&attribution};

  std::future<ProcessUtil::Result> run = std::async(std::launch::async, [&] {
    return std::get<0>(
        stdexec::sync_wait(
            ProcessUtil::run(io, std::filesystem::temp_directory_path(),
                             "cmake -E sleep 2", LaunchRegistrar(registering)))
            .value());
  });

  // Resolved while it runs.
  std::optional<AttemptId> resolved;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!resolved.has_value() && std::chrono::steady_clock::now() < deadline) {
    if (const std::uint32_t pid = registering.pid; pid != 0) {
      resolved = attribution.resolve(pid);
    }
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(resolved, k_attempt);

  const ProcessUtil::Result result = run.get();
  ASSERT_TRUE(result.has_value()) << result.error().message();

  // Its registration went with the run.
  EXPECT_EQ(attribution.resolve(registering.pid), std::nullopt);
}

TEST(ProcessAttribution, ResolvesNothingForAProcessNoCommandStarted) {
  ProcessAttribution attribution;
  EXPECT_EQ(attribution.resolve(ProcessInfo::self()), std::nullopt);
  EXPECT_EQ(attribution.resolve(0), std::nullopt);
}

}  // namespace
