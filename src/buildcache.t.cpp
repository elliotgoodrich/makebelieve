// SPDX-License-Identifier: MIT
#include "buildcache.hpp"

#include "directorytreeutil.hpp"
#include "filesystemutil.hpp"
#include "nativefile.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace makebelieve;

// All of what @a file reads, at once.
std::string read_all(const OpenedFile& file) {
  return DirectoryTreeUtil::wait_read(file.read(0, std::size_t{1} << 20))
      .value();
}

// The content @a cache holds under @a hash, as `open` finds it.
std::expected<std::string, std::error_code> read(const BuildCache& cache,
                                                 std::string_view hash) {
  const auto opened = cache.open(hash);
  if (!opened.has_value()) {
    return std::unexpected(opened.error());
  }
  return read_all(**opened);
}

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

TEST_F(BuildCacheTest, MarksItsDirectoryAsACache) {
  { const BuildCache cache(directory); }

  const std::expected<std::string, std::error_code> gitignore =
      FileSystemUtil::read_file(directory / ".gitignore");
  ASSERT_TRUE(gitignore.has_value()) << gitignore.error().message();
  EXPECT_TRUE(gitignore->ends_with("\n*\n")) << *gitignore;

  const std::expected<std::string, std::error_code> tag =
      FileSystemUtil::read_file(directory / "CACHEDIR.TAG");
  ASSERT_TRUE(tag.has_value()) << tag.error().message();
  EXPECT_TRUE(tag->starts_with("Signature: 8a477f597d28d172789f06886806bc55"))
      << *tag;
}

TEST_F(BuildCacheTest, KeepsAGitignoreThatIsAlreadyThere) {
  std::filesystem::create_directories(directory);
  ASSERT_FALSE(FileSystemUtil::write_file(directory / ".gitignore", "mine\n"));

  { const BuildCache cache(directory); }

  const std::expected<std::string, std::error_code> gitignore =
      FileSystemUtil::read_file(directory / ".gitignore");
  ASSERT_TRUE(gitignore.has_value()) << gitignore.error().message();
  EXPECT_EQ(*gitignore, "mine\n");
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
  EXPECT_EQ(read(cache, hash), "built");
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
  EXPECT_FALSE(read(cache, BuildCache::hash("first")).has_value());
}

TEST_F(BuildCacheTest, OutputsWithTheSameContentShareIt) {
  BuildCache cache(directory);
  const std::string hash = cache.store(run("x", "c"), "same", k_built, {});
  EXPECT_EQ(cache.store(run("y", "c"), "same", k_built, {}), hash);
  EXPECT_EQ(object_count(), 1U);

  // Content stays for as long as any record names it.
  cache.forget(std::vector<std::filesystem::path>{"x"});
  EXPECT_EQ(read(cache, hash), "same");
  cache.forget(std::vector<std::filesystem::path>{"y"});
  EXPECT_FALSE(read(cache, hash).has_value());
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

  const std::expected<std::string, std::error_code> content = read(cache, hash);
  ASSERT_FALSE(content.has_value());
  EXPECT_EQ(content.error(), std::errc::no_such_file_or_directory);
}

TEST_F(BuildCacheTest, ReadingAnythingButAHashFails) {
  const BuildCache cache(directory);
  EXPECT_FALSE(read(cache, "../cache.sqlite").has_value());
  EXPECT_FALSE(read(cache, "").has_value());
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
  EXPECT_EQ(read(cache, hash), "kept");
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

// Hashing through an opened file names content as hashing its bytes does.
TEST_F(BuildCacheTest, HashesAnOpenedFileAsItsBytes) {
  BuildCache cache(directory);
  const std::string hash = cache.store(run("x", "c"), "anything", k_built, {});
  const auto opened = cache.open(hash);
  ASSERT_TRUE(opened.has_value()) << opened.error().message();
  EXPECT_EQ(BuildCache::hash(**opened), hash);
}

// Whatever a run left in scratch is gone by the next.
TEST_F(BuildCacheTest, OpeningEmptiesTheScratchDirectory) {
  {
    const BuildCache cache(directory);
    EXPECT_TRUE(std::filesystem::is_directory(cache.scratch_directory()));
    ASSERT_FALSE(FileSystemUtil::write_file(
        cache.scratch_directory() / "left.out", "left"));
  }
  const BuildCache cache(directory);
  EXPECT_TRUE(std::filesystem::is_empty(cache.scratch_directory()));
}

// A file stored is moved in as the content - or deleted, if that content is
// there already - and gone from where it was either way.
TEST_F(BuildCacheTest, StoringAFileTakesItOver) {
  BuildCache cache(directory);
  const std::filesystem::path first = cache.scratch_directory() / "1.out";
  const std::filesystem::path second = cache.scratch_directory() / "2.out";
  ASSERT_FALSE(FileSystemUtil::write_file(first, "same"));
  ASSERT_FALSE(FileSystemUtil::write_file(second, "same"));
  const std::string hash = BuildCache::hash("same");

  cache.store_file(run("x", "c"), first, hash, k_built, {});
  cache.store_file(run("y", "c"), second, hash, k_built, {});

  EXPECT_FALSE(std::filesystem::exists(first));
  EXPECT_FALSE(std::filesystem::exists(second));
  EXPECT_EQ(object_count(), 1U);
  EXPECT_EQ(read(cache, hash), "same");
  EXPECT_EQ(cache.records().size(), 2U);
}

// Content that something has open is never rewritten, and that open reads on
// once the content is dropped - and the same content can be stored again.
TEST_F(BuildCacheTest, OpenContentReadsOnOnceItIsDropped) {
  BuildCache cache(directory);
  const std::string hash = cache.store(run("x", "c"), "kept", k_built, {});
  const auto opened = cache.open(hash);
  ASSERT_TRUE(opened.has_value()) << opened.error().message();

  cache.store(run("y", "c"), "kept", k_built, {});
  cache.forget(std::vector<std::filesystem::path>{"x", "y"});
  EXPECT_EQ(object_count(), 0U);
  EXPECT_EQ(read_all(**opened), "kept");

  const std::filesystem::path again = cache.scratch_directory() / "again.out";
  ASSERT_FALSE(FileSystemUtil::write_file(again, "kept"));
  cache.store_file(run("x", "c"), again, hash, k_built, {});
  EXPECT_EQ(read(cache, hash), "kept");
  EXPECT_EQ(read_all(**opened), "kept");
}

// Content read from an opened file is written in only if it is not there.
TEST_F(BuildCacheTest, StoresWhatAnOpenedFileReads) {
  BuildCache cache(directory);
  const std::filesystem::path source = directory / "source.txt";
  ASSERT_FALSE(FileSystemUtil::write_file(source, "copied"));
  const auto opened = NativeFile::open(source);
  ASSERT_TRUE(opened.has_value());

  cache.store_content(run("x", "c"), **opened, BuildCache::hash("copied"),
                      k_built, {});
  cache.store_content(run("y", "c"), **opened, BuildCache::hash("copied"),
                      k_built, {});

  EXPECT_EQ(object_count(), 1U);
  EXPECT_EQ(read(cache, BuildCache::hash("copied")), "copied");
  EXPECT_EQ(cache.records().size(), 2U);
}

}  // namespace
