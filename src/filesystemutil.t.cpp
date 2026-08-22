// SPDX-License-Identifier: MIT
#include "filesystemutil.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

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
    FileSystemUtil, AreDisjoint,
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

}  // namespace
