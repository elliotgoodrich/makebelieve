// SPDX-License-Identifier: MIT
#include "filesystemutil.hpp"

#include <gtest/gtest.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

namespace {

// are_disjoint requires absolute inputs, and what counts as absolute is
// platform-specific: a POSIX root on Linux, a drive-qualified root on Windows.
// Anchoring the same relative fragments to a platform root is what lets one set
// of cases exercise the logic on both.
std::filesystem::path rooted(std::string_view fragment) {
#ifdef _WIN32
  return std::filesystem::path("C:\\") / std::filesystem::path(fragment);
#else
  return std::filesystem::path("/") / std::filesystem::path(fragment);
#endif
}

// One case for are_disjoint: two paths and whether they should be reported as
// unrelated (neither equal to nor nested inside the other). Absolute paths
// throughout so weakly_canonical normalises them without consulting the current
// directory, keeping the expectations independent of where the test runs.
struct DisjointCase {
  std::filesystem::path a;
  std::filesystem::path b;
  bool expected;
};

class AreDisjoint : public ::testing::TestWithParam<DisjointCase> {};

TEST_P(AreDisjoint, MatchesExpectation) {
  const DisjointCase& c = GetParam();
  EXPECT_EQ(makebelieve::FileSystemUtil::are_disjoint(c.a, c.b), c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    FileSystemUtil,
    AreDisjoint,
    ::testing::Values(
        // A parent/child pair overlaps whichever way round it is passed - the
        // two orderings prove the symmetry.
        DisjointCase{rooted("src/build"), rooted("src"), false},
        DisjointCase{rooted("src"), rooted("src/build"), false},
        // A deeper descendant still overlaps.
        DisjointCase{rooted("src"), rooted("src/a/b/c"), false},
        // `..` segments are normalised away first, so a path that spells its
        // way back inside is still an overlap, not disjoint.
        DisjointCase{rooted("src/build/../out"), rooted("src"), false},
        // Equal paths overlap - not disjoint.
        DisjointCase{rooted("src"), rooted("src"), false},
        // A shared string prefix is not a shared parent: these are disjoint.
        DisjointCase{rooted("source-tree"), rooted("src"), true},
        // Unrelated subtrees.
        DisjointCase{rooted("a/b"), rooted("c/d"), true}));

// A path in the temporary directory named for the running test.
std::filesystem::path temp_file() {
  return std::filesystem::temp_directory_path() /
         ("makebelieve-fsutil-" +
          std::string(
              ::testing::UnitTest::GetInstance()->current_test_info()->name()));
}

TEST(ReadFile, ReadsEveryByteOfAFileLargerThanOneRead) {
  std::string contents;
  for (int i = 0; i < 10000; ++i) {
    contents += static_cast<char>(i % 256);  // NUL, CR and LF among them
  }
  const std::filesystem::path path = temp_file();
  {
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
  }

  const std::expected<std::string, std::error_code> read =
      makebelieve::FileSystemUtil::read_file(path);
  std::filesystem::remove(path);

  ASSERT_TRUE(read.has_value()) << read.error().message();
  EXPECT_EQ(*read, contents);
}

TEST(ReadFile, ReadsAnEmptyFile) {
  const std::filesystem::path path = temp_file();
  std::ofstream(path, std::ios::binary).close();

  const std::expected<std::string, std::error_code> read =
      makebelieve::FileSystemUtil::read_file(path);
  std::filesystem::remove(path);

  ASSERT_TRUE(read.has_value()) << read.error().message();
  EXPECT_EQ(*read, "");
}

TEST(ReadFile, ReportsAMissingFile) {
  const std::expected<std::string, std::error_code> read =
      makebelieve::FileSystemUtil::read_file(temp_file());

  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), std::errc::no_such_file_or_directory);
}

#ifdef __linux__
// /proc files report a size of 0, so only reading to the end finds what they
// hold.
TEST(ReadFile, ReadsAProcFile) {
  const std::expected<std::string, std::error_code> read =
      makebelieve::FileSystemUtil::read_file("/proc/self/status");

  ASSERT_TRUE(read.has_value()) << read.error().message();
  EXPECT_TRUE(read->starts_with("Name:")) << *read;
}
#endif

}  // namespace
