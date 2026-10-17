// SPDX-License-Identifier: MIT
#include "osbuildprocesstracker.hpp"

#include "buildprocesstracker.hpp"
#include "iocontext.hpp"
#include "processinfo.hpp"
#include "processutil.hpp"

#include <gtest/gtest.h>
#include <stdexec/execution.hpp>

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

// The output the command is added to the tracker as building.
const std::filesystem::path k_output = "out/built.txt";

// Forwards to an OsBuildProcessTracker, remembering the pid of the last
// process added.
struct Forwarding final : BuildProcessTracker {
  OsBuildProcessTracker* tracker;
  std::atomic<std::uint32_t> pid = 0;

  explicit Forwarding(OsBuildProcessTracker& target) : tracker(&target) {}

  std::expected<Token, std::error_code> add(
      const std::filesystem::path& output,
      const ProcessHandle& process) override {
    pid = process.pid;
    return tracker->add(output, process);
  }

  void remove(Token token) noexcept override { tracker->remove(token); }

  std::optional<std::filesystem::path> find(
      std::uint32_t requester_pid) override {
    return tracker->find(requester_pid);
  }
};

TEST(OsBuildProcessTracker, FindsACommandsBuildOnlyWhileItIsTracked) {
  IoContext io;
  OsBuildProcessTracker tracker;
  Forwarding forwarding(tracker);

  std::future<ProcessUtil::Result> run = std::async(std::launch::async, [&] {
    return std::get<0>(
        stdexec::sync_wait(
            ProcessUtil::run(io, std::filesystem::temp_directory_path(),
                             "cmake -E sleep 2", forwarding, k_output, {}))
            .value());
  });

  // Found while it runs.
  std::optional<std::filesystem::path> found;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!found.has_value() && std::chrono::steady_clock::now() < deadline) {
    if (const std::uint32_t pid = forwarding.pid; pid != 0) {
      found = tracker.find(pid);
    }
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(found, k_output);

  const ProcessUtil::Result result = run.get();
  ASSERT_TRUE(result.has_value()) << result.error().message();

  // It was removed from the tracker with the run.
  EXPECT_EQ(tracker.find(forwarding.pid), std::nullopt);
}

TEST(OsBuildProcessTracker, FindsNothingForAProcessNoCommandStarted) {
  OsBuildProcessTracker tracker;
  EXPECT_EQ(tracker.find(ProcessInfo::self()), std::nullopt);
  EXPECT_EQ(tracker.find(0), std::nullopt);
}

}  // namespace
