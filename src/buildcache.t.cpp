// SPDX-License-Identifier: MIT
#include "buildcache.hpp"

#include "filesystemutil.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace makebelieve;

class BuildCacheTest : public ::testing::Test {
 protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("makebelieve-buildcache-" +
       std::string(
           ::testing::UnitTest::GetInstance()->current_test_info()->name()));

  void SetUp() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }

  [[nodiscard]] std::size_t object_count() const {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator(directory / "objects")) {
      ++count;
    }
    return count;
  }

  static Command run(std::filesystem::path output, std::string text) {
    return Command{.output = std::move(output),
                   .action = Manifest::Action::Run,
                   .text = std::move(text)};
  }

  static const std::chrono::file_clock::time_point k_built;
};

const std::chrono::file_clock::time_point BuildCacheTest::k_built =
    std::chrono::file_clock::time_point(
        std::chrono::file_clock::duration(123456789));

TEST_F(BuildCacheTest, AFreshCacheIsEmpty) {
  const BuildCache cache(directory);
  EXPECT_TRUE(cache.records().empty());
  EXPECT_TRUE(std::filesystem::is_directory(directory / "objects"));
}

TEST_F(BuildCacheTest, StoredRecordsOutliveTheCache) {
  const std::vector<BuildCache::Input> inputs = {
      {.path = "src/a.txt", .mtime = 42, .size = 7, .hash = {}},
      {.path = "src", .mtime = 43, .size = -1, .hash = {}},
      {.path = "@/gen/b.txt",
       .mtime = 0,
       .size = 0,
       .hash = BuildCache::hash("b")},
  };
  std::string hash;
  {
    BuildCache cache(directory);
    hash = cache.store(run("out/x.txt", "cc %out"), "built", k_built, inputs);
  }

  const BuildCache cache(directory);
  const std::vector<BuildCache::Record> records = cache.records();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0],
            (BuildCache::Record{.command = run("out/x.txt", "cc %out"),
                                .hash = hash,
                                .mtime = k_built,
                                .inputs = inputs}));
  EXPECT_EQ(hash, BuildCache::hash("built"));
  EXPECT_EQ(cache.read(hash), "built");
}

TEST_F(BuildCacheTest, KeepsEveryAction) {
  BuildCache cache(directory);
  for (const Manifest::Action action :
       {Manifest::Action::Run, Manifest::Action::Capture,
        Manifest::Action::Copy}) {
    cache.store(Command{.output = "x", .action = action, .text = "t"}, "b",
                k_built, {});
    ASSERT_EQ(cache.records().size(), 1U);
    EXPECT_EQ(cache.records()[0].command.action, action);
  }
}

TEST_F(BuildCacheTest, KeepsPathsThatAreNotAscii) {
  const std::filesystem::path output(u8"café/日本.txt");
  BuildCache cache(directory);
  cache.store(run(output, "x"), "b", k_built,
              std::vector<BuildCache::Input>{{.path = output}});

  const std::vector<BuildCache::Record> records = cache.records();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].command.output, output);
  ASSERT_EQ(records[0].inputs.size(), 1U);
  EXPECT_EQ(records[0].inputs[0].path, output);
}

TEST_F(BuildCacheTest, StoringAgainReplacesTheRecordAndDropsOldContent) {
  BuildCache cache(directory);
  cache.store(run("x", "one"), "first", k_built,
              std::vector<BuildCache::Input>{{.path = "a"}});
  const std::string hash =
      cache.store(run("x", "two"), "second", k_built,
                  std::vector<BuildCache::Input>{{.path = "b"}});

  const std::vector<BuildCache::Record> records = cache.records();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].command.text, "two");
  EXPECT_EQ(records[0].hash, hash);
  ASSERT_EQ(records[0].inputs.size(), 1U);
  EXPECT_EQ(records[0].inputs[0].path, "b");
  EXPECT_EQ(object_count(), 1U);
  EXPECT_FALSE(cache.read(BuildCache::hash("first")).has_value());
}

TEST_F(BuildCacheTest, OutputsWithTheSameContentShareIt) {
  BuildCache cache(directory);
  const std::string hash = cache.store(run("x", "c"), "same", k_built, {});
  EXPECT_EQ(cache.store(run("y", "c"), "same", k_built, {}), hash);
  EXPECT_EQ(object_count(), 1U);

  // Content stays for as long as any record names it.
  cache.forget(std::vector<std::filesystem::path>{"x"});
  EXPECT_EQ(cache.read(hash), "same");
  cache.forget(std::vector<std::filesystem::path>{"y"});
  EXPECT_FALSE(cache.read(hash).has_value());
  EXPECT_EQ(object_count(), 0U);
}

TEST_F(BuildCacheTest, ForgetDropsOnlyTheOutputsNamed) {
  BuildCache cache(directory);
  cache.store(run("x", "c"), "x", k_built,
              std::vector<BuildCache::Input>{{.path = "a"}});
  cache.store(run("y", "c"), "y", k_built,
              std::vector<BuildCache::Input>{{.path = "a"}});

  cache.forget(std::vector<std::filesystem::path>{"x", "never-stored"});

  const std::vector<BuildCache::Record> records = cache.records();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].command.output, "y");
  EXPECT_EQ(records[0].inputs.size(), 1U);
}

TEST_F(BuildCacheTest, ContentThatNoLongerMatchesItsHashReadsAsMissing) {
  BuildCache cache(directory);
  const std::string hash = cache.store(run("x", "c"), "content", k_built, {});
  ASSERT_FALSE(
      FileSystemUtil::write_file(directory / "objects" / hash, "conten"));

  const std::expected<std::string, std::error_code> read = cache.read(hash);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), std::errc::no_such_file_or_directory);
}

TEST_F(BuildCacheTest, ReadingAnythingButAHashFails) {
  const BuildCache cache(directory);
  EXPECT_FALSE(cache.read("../cache.sqlite").has_value());
  EXPECT_FALSE(cache.read("").has_value());
}

TEST_F(BuildCacheTest, GarbageCollectionKeepsOnlyRecordedContent) {
  BuildCache cache(directory);
  const std::string hash = cache.store(run("x", "c"), "kept", k_built, {});
  ASSERT_FALSE(FileSystemUtil::write_file(
      directory / "objects" / BuildCache::hash("stray"), "stray"));
  ASSERT_FALSE(FileSystemUtil::write_file(
      directory / "objects" / (hash + ".7.tmp"), "half"));

  cache.collect_garbage();

  EXPECT_EQ(object_count(), 1U);
  EXPECT_EQ(cache.read(hash), "kept");
}

TEST_F(BuildCacheTest, ASecondCacheOnTheSameDirectoryFails) {
  const BuildCache cache(directory);
  try {
    const BuildCache second(directory);
    FAIL() << "a second cache opened";
  } catch (const std::system_error& error) {
    EXPECT_EQ(error.code(), std::errc::device_or_resource_busy);
  }
}

TEST_F(BuildCacheTest, TheDirectoryIsFreeOnceTheCacheGoes) {
  { const BuildCache cache(directory); }
  EXPECT_NO_THROW(const BuildCache cache(directory));
}

TEST_F(BuildCacheTest, HashesAreThirtyTwoHexDigits) {
  const std::string hash = BuildCache::hash("anything");
  EXPECT_EQ(hash.size(), 32U);
  EXPECT_EQ(hash.find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_NE(hash, BuildCache::hash("anything else"));
}

}  // namespace
