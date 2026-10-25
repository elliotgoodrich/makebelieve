// SPDX-License-Identifier: MIT
#include "unmountchannel.hpp"

#include "iocontext.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>

using namespace makebelieve;

namespace {

class UnmountChannelTest : public ::testing::Test {
 protected:
  IoContext io;

  // Stands in for a mountpoint: the channel only needs it to exist. Unique to
  // this run, as the endpoint is named from it, and other runs - from other
  // build directories, say - may be listening at the same time.
  std::filesystem::path mountpoint =
      std::filesystem::temp_directory_path() /
      ("makebelieve-test-" +
       std::string(
           ::testing::UnitTest::GetInstance()->current_test_info()->name()) +
       "-" + std::to_string(std::random_device()()));

  void SetUp() override {
    std::error_code ec;
    std::filesystem::remove_all(mountpoint, ec);
    std::filesystem::create_directories(mountpoint);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(mountpoint, ec);
  }
};

// Waits for @a token to be signalled, up to a generous timeout.
bool wait_for_stop(const std::stop_token& token) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!token.stop_requested()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

TEST_F(UnmountChannelTest, ATokenIsUnsetBeforeAnyRequest) {
  UnmountChannel channel;
  const std::stop_token before = channel.token();
  channel.listen(mountpoint, io);
  EXPECT_TRUE(before.stop_possible());
  EXPECT_FALSE(before.stop_requested());
  EXPECT_FALSE(channel.token().stop_requested());
}

TEST_F(UnmountChannelTest, NothingListeningIsNotMounted) {
  const UnmountChannel channel;
  EXPECT_EQ(UnmountChannel::request_unmount(mountpoint),
            UnmountChannel::Result::not_mounted);
}

TEST_F(UnmountChannelTest, ASecondChannelOnTheSameMountpointThrows) {
  UnmountChannel first;
  first.listen(mountpoint, io);
  UnmountChannel second;
  try {
    second.listen(mountpoint, io);
    ADD_FAILURE() << "a second channel listened";
  } catch (const std::system_error& error) {
    // Also a std::runtime_error, but for failing to make the endpoint.
    ADD_FAILURE() << "could not make the endpoint: " << error.what();
  } catch (const std::runtime_error&) {  // NOLINT(bugprone-empty-catch)
    // Taken, as expected.
  }
}

TEST_F(UnmountChannelTest, AnUnmountSignalsATokenTakenBeforeListening) {
  // Declared before the channel, so a failed assertion destroys the channel
  // first, letting the request return, rather than waiting on it forever.
  std::atomic<bool> returned = false;
  std::future<UnmountChannel::Result> result;

  std::optional<UnmountChannel> channel(std::in_place);
  const std::stop_token token = channel->token();
  channel->listen(mountpoint, io);

  result = std::async(std::launch::async, [&] {
    const UnmountChannel::Result r =
        UnmountChannel::request_unmount(mountpoint);
    returned = true;
    return r;
  });

  ASSERT_TRUE(wait_for_stop(token));

  // Signalled, but held until the channel goes, however long the mount takes
  // to tear down.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(returned);

  channel.reset();
  ASSERT_EQ(result.wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  EXPECT_EQ(result.get(), UnmountChannel::Result::ok);

  // Everything is let go, so the next mount can serve straight away.
  UnmountChannel next;
  EXPECT_NO_THROW(next.listen(mountpoint, io));
}

}  // namespace
