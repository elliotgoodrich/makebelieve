// SPDX-License-Identifier: MIT
#include "filesnapshots.hpp"

#include "inmemorydirectorytree.hpp"

#include <gtest/gtest.h>
#include <stdexec/execution.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

using namespace makebelieve;

namespace {

using namespace std::chrono_literals;

std::expected<std::shared_ptr<const FileSnapshot>, std::error_code> take(
    const FileSnapshots& snapshots,
    const std::filesystem::path& path) {
  auto result = stdexec::sync_wait(snapshots.take(path));
  return std::get<0>(std::move(result).value());
}

// Content big enough to take several reads to drain.
std::string large_contents() {
  std::string content(200'000, '\0');
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>('a' + (i * 7 % 26));
  }
  return content;
}

}  // namespace

TEST(FileSnapshots, TakesTheWholeFileAsItIsNow) {
  InMemoryDirectoryTree tree;
  const std::string content = large_contents();
  const auto mtime = std::chrono::file_clock::now();
  tree.write_file("a.bin", content, mtime);
  const FileSnapshots snapshots(tree);

  const auto snapshot = take(snapshots, "a.bin");
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().message();
  EXPECT_TRUE((*snapshot)->content() == content);
  EXPECT_EQ((*snapshot)->mtime, mtime);
  EXPECT_EQ((*snapshot)->info().size, content.size());
}

// A tree that keeps its files in memory hands out its own content, which the
// snapshot then shares rather than copies.
TEST(FileSnapshots, SharesTheTreesOwnContent) {
  InMemoryDirectoryTree tree;
  tree.write_file("a.bin", large_contents());
  const FileSnapshots snapshots(tree);

  const auto snapshot = take(snapshots, "a.bin");
  auto result = stdexec::sync_wait(tree.read_whole("a.bin"));
  ASSERT_TRUE(snapshot.has_value());
  const auto& whole = std::get<0>(result.value());
  ASSERT_TRUE(whole.has_value());
  EXPECT_EQ((*snapshot)->bytes, whole->bytes);
}

TEST(FileSnapshots, SharesOneSnapshotOfAVersionWhileItIsHeld) {
  InMemoryDirectoryTree tree;
  tree.write_file("a.txt", "abc");
  const FileSnapshots snapshots(tree);

  const auto first = take(snapshots, "a.txt");
  const auto second = take(snapshots, "a.txt");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second);
}

// A holder keeps the version it took. The next take is of the file as it now
// is, whether or not the change was reported: the tree itself says it moved.
TEST(FileSnapshots, AHolderKeepsItsVersionWhenTheFileChanges) {
  InMemoryDirectoryTree tree;
  tree.write_file("a.txt", "old");
  FileSnapshots snapshots(tree);
  const auto old_snapshot = take(snapshots, "a.txt");
  ASSERT_TRUE(old_snapshot.has_value());

  tree.write_file("a.txt", "new", std::chrono::file_clock::now() + 1s);
  const auto unreported = take(snapshots, "a.txt");
  ASSERT_TRUE(unreported.has_value());
  EXPECT_EQ((*unreported)->content(), "new");

  tree.write_file("a.txt", "NEW", std::chrono::file_clock::now() + 2s);
  snapshots.changed({.entries_changed = {"a.txt"}});
  const auto reported = take(snapshots, "a.txt");
  ASSERT_TRUE(reported.has_value());
  EXPECT_EQ((*reported)->content(), "NEW");

  EXPECT_EQ((*old_snapshot)->content(), "old");
}

TEST(FileSnapshots, NumbersEachReportedChange) {
  InMemoryDirectoryTree tree;
  tree.write_file("a.txt", "a");
  tree.write_file("b.txt", "b");
  FileSnapshots snapshots(tree);
  EXPECT_EQ(snapshots.generation("a.txt"), 0U);

  snapshots.changed({.entries_changed = {"a.txt"}});
  const std::uint64_t a = snapshots.generation("a.txt");
  EXPECT_GT(a, 0U);
  EXPECT_EQ(snapshots.generation("b.txt"), 0U);

  const auto snapshot = take(snapshots, "a.txt");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ((*snapshot)->generation, a);

  snapshots.changed({.everything_dirty = true});
  EXPECT_GT(snapshots.generation("a.txt"), a);
  EXPECT_GT(snapshots.generation("b.txt"), 0U);
}

TEST(FileSnapshots, FailsAsTheTreeDoes) {
  InMemoryDirectoryTree tree;
  tree.make_directory("sub");
  const FileSnapshots snapshots(tree);
  EXPECT_EQ(take(snapshots, "missing.txt").error(),
            std::errc::no_such_file_or_directory);
  EXPECT_EQ(take(snapshots, "sub").error(), std::errc::is_a_directory);
}

// A snapshot nobody holds any more is forgotten, rather than left behind for a
// file that may never be opened again.
TEST(FileSnapshots, ForgetsASnapshotOnceNobodyHoldsIt) {
  InMemoryDirectoryTree tree;
  tree.write_file("a.txt", "abc");
  tree.write_file("b.txt", "def");
  const FileSnapshots snapshots(tree);

  auto a = take(snapshots, "a.txt");
  auto b = take(snapshots, "b.txt");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(snapshots.held(), 2U);
  a->reset();
  EXPECT_EQ(snapshots.held(), 1U);
  b->reset();
  EXPECT_EQ(snapshots.held(), 0U);
}

// Letting go of an old version of a file leaves the newer one there to share.
TEST(FileSnapshots, LettingGoOfAnOldVersionKeepsTheNewerOne) {
  InMemoryDirectoryTree tree;
  tree.write_file("a.txt", "old");
  FileSnapshots snapshots(tree);
  auto old_snapshot = take(snapshots, "a.txt");
  tree.write_file("a.txt", "new", std::chrono::file_clock::now() + 1s);
  snapshots.changed({.entries_changed = {"a.txt"}});
  const auto new_snapshot = take(snapshots, "a.txt");
  ASSERT_TRUE(old_snapshot.has_value());
  ASSERT_TRUE(new_snapshot.has_value());

  old_snapshot->reset();
  EXPECT_EQ(snapshots.held(), 1U);
  const auto again = take(snapshots, "a.txt");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, *new_snapshot);
}

// A snapshot can outlive what took it - a handle closed after the filesystem
// that opened it has gone, say.
TEST(FileSnapshots, ASnapshotCanOutliveWhatTookIt) {
  InMemoryDirectoryTree tree;
  tree.write_file("a.txt", "abc");
  std::optional<FileSnapshots> snapshots(std::in_place, tree);
  auto snapshot = take(*snapshots, "a.txt");
  snapshots.reset();

  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ((*snapshot)->content(), "abc");
  EXPECT_EQ((*snapshot)->path, "a.txt");
  snapshot->reset();
}
