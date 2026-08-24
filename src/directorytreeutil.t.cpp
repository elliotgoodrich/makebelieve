// SPDX-License-Identifier: MIT
#include "directorytreeutil.hpp"

#include "inmemorydirectorytree.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

// Named for the type under test so TEST_F reads as DirectoryTreeUtil. That
// takes the name, so the type under test is spelled makebelieve:: throughout
// this file.
class DirectoryTreeUtil : public ::testing::Test {
 protected:
  makebelieve::InMemoryDirectoryTree a;
  makebelieve::InMemoryDirectoryTree b;
};

TEST_F(DirectoryTreeUtil, TwoEmptyTreesAreEqual) {
  EXPECT_TRUE(makebelieve::DirectoryTreeUtil::equal(a, b));
  EXPECT_EQ(makebelieve::DirectoryTreeUtil::diff(a, b), "");
}

TEST_F(DirectoryTreeUtil, IdenticalContentIsEqual) {
  a.write_file("hello.txt", "hello world");
  a.make_directory("sub");
  a.write_file("sub/nested.txt", "nested");

  b.write_file("hello.txt", "hello world");
  b.make_directory("sub");
  b.write_file("sub/nested.txt", "nested");

  EXPECT_TRUE(makebelieve::DirectoryTreeUtil::equal(a, b));
}

// mtimes are never equal by construction (each write_file()/make_directory()
// call below defaults to a fresh std::chrono::file_clock::now()), so this
// doubles as proof that mtime is not part of the comparison.
TEST_F(DirectoryTreeUtil, MtimeIsIgnored) {
  a.write_file("hello.txt", "hello world");
  b.write_file("hello.txt", "hello world");

  EXPECT_TRUE(makebelieve::DirectoryTreeUtil::equal(a, b));
}

TEST_F(DirectoryTreeUtil, DifferingFileContentIsNotEqual) {
  a.write_file("hello.txt", "hello world");
  b.write_file("hello.txt", "goodbye world");

  EXPECT_FALSE(makebelieve::DirectoryTreeUtil::equal(a, b));
  EXPECT_EQ(makebelieve::DirectoryTreeUtil::diff(a, b),
            "hello.txt: content differs (11 bytes in a, 13 bytes in b)");
}

TEST_F(DirectoryTreeUtil, MissingFromOneTreeIsNotEqual) {
  a.write_file("only_in_a.txt", "x");

  const std::string diff = makebelieve::DirectoryTreeUtil::diff(a, b);
  EXPECT_FALSE(diff.empty());
  EXPECT_NE(diff.find("only_in_a.txt"), std::string::npos);
}

TEST_F(DirectoryTreeUtil, FileVersusDirectoryIsNotEqual) {
  a.write_file("entry", "content");
  b.make_directory("entry");

  EXPECT_EQ(makebelieve::DirectoryTreeUtil::diff(a, b),
            "entry: is a file in a and a directory in b");
}

TEST_F(DirectoryTreeUtil, DifferingChildrenIsNotEqual) {
  a.make_directory("sub");
  a.write_file("sub/one.txt", "1");
  b.make_directory("sub");
  b.write_file("sub/two.txt", "2");

  EXPECT_EQ(makebelieve::DirectoryTreeUtil::diff(a, b),
            "sub: children differ (a has [one.txt], b has [two.txt])");
}

TEST_F(DirectoryTreeUtil, DifferenceIsFoundInsideAMatchingDirectory) {
  a.make_directory("sub");
  a.write_file("sub/nested.txt", "a-content");
  b.make_directory("sub");
  b.write_file("sub/nested.txt", "b-content");

  EXPECT_EQ(makebelieve::DirectoryTreeUtil::diff(a, b),
            "sub/nested.txt: content differs (9 bytes in a, 9 bytes in b)");
}

}  // namespace
