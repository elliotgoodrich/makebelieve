// SPDX-License-Identifier: MIT
#include "inmemorydirectorytree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

namespace {

void expect_file(const makebelieve::EntryInfo& info,
                 std::size_t expected_size) {
  ASSERT_TRUE(std::holds_alternative<makebelieve::FileInfo>(info))
      << "expected a file, got a directory";
  EXPECT_EQ(std::get<makebelieve::FileInfo>(info).size, expected_size);
}

void expect_directory(const makebelieve::EntryInfo& info) {
  EXPECT_TRUE(std::holds_alternative<makebelieve::DirectoryInfo>(info))
      << "expected a directory, got a file";
}

// ls() order is unspecified, so every listing assertion goes through here
// rather than indexing the raw vector.
std::vector<std::string> sorted_names(
    const std::vector<makebelieve::TreeEntry>& entries) {
  std::vector<std::string> names;
  names.reserve(entries.size());
  for (const makebelieve::TreeEntry& entry : entries) {
    names.push_back(entry.name.string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> sorted_paths(
    const std::vector<std::filesystem::path>& paths) {
  std::vector<std::string> names;
  names.reserve(paths.size());
  for (const std::filesystem::path& path : paths) {
    names.push_back(path.generic_string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

// Named for the type under test so TEST_F reads as InMemoryDirectoryTree.
// That takes the name, so the class under test is spelled makebelieve::
// throughout this file.
class InMemoryDirectoryTree : public ::testing::Test {
 protected:
  void SetUp() override {
    m_tree.write_file("hello.txt", "hello world");
    m_tree.write_file("empty.txt", "");
    m_tree.write_file("binary.bin",
                      std::string{'\r', '\n', '\0', static_cast<char>(0xff)});
    m_tree.make_directory("sub");
    m_tree.write_file("sub/nested.txt", "nested");
    m_tree.make_directory("sub/deeper");
  }

  makebelieve::InMemoryDirectoryTree& tree() { return m_tree; }

 private:
  makebelieve::InMemoryDirectoryTree m_tree;
};

// ---------------------------------------------------------------------------
// Paths that escape the root. status() and ls() collapse every failure into
// one code, so one table can assert the same expectation across all four
// calls, matching RealDirectoryTree's contract.
// ---------------------------------------------------------------------------

struct EscapeCase {
  std::string_view label;
  std::filesystem::path input;
};

class EscapingPath : public InMemoryDirectoryTree,
                     public ::testing::WithParamInterface<EscapeCase> {};

TEST_P(EscapingPath, IsRejectedByEveryOperation) {
  const EscapeCase& c = GetParam();

  const std::expected<makebelieve::EntryInfo, std::error_code> status =
      tree().status(c.input);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error(), std::errc::no_such_file_or_directory);

  const std::expected<std::vector<makebelieve::TreeEntry>, std::error_code>
      listing = tree().ls(c.input);
  ASSERT_FALSE(listing.has_value());
  EXPECT_EQ(listing.error(), std::errc::no_such_file_or_directory);

  const std::expected<std::string, std::error_code> read =
      tree().read(c.input, 0, 4);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), std::errc::no_such_file_or_directory);

  // Check 0-size read (that short circuits) still returns an error.
  const std::expected<std::string, std::error_code> empty_read =
      tree().read(c.input, 0, 0);
  ASSERT_FALSE(empty_read.has_value());
  EXPECT_EQ(empty_read.error(), std::errc::no_such_file_or_directory);
}

INSTANTIATE_TEST_SUITE_P(
    InMemoryDirectoryTree,
    EscapingPath,
    ::testing::Values(EscapeCase{"parent", ".."},
                      EscapeCase{"parent_file", "../outside.txt"},
                      EscapeCase{"through_a_directory",
                                 "sub/../../outside.txt"},
                      EscapeCase{"interior_segments_collapse", "sub/../.."},
                      EscapeCase{"root_directory", "/absolute.txt"}),
    [](const ::testing::TestParamInfo<EscapeCase>& info) {
      return std::string(info.param.label);
    });

// ---------------------------------------------------------------------------
// status()
// ---------------------------------------------------------------------------

enum class Kind { File, Directory, Error };

struct StatusCase {
  std::string_view label;
  std::filesystem::path input;
  Kind expected;
  std::size_t size;
};

class StatusEntry : public InMemoryDirectoryTree,
                    public ::testing::WithParamInterface<StatusCase> {};

TEST_P(StatusEntry, MatchesExpectation) {
  const StatusCase& c = GetParam();
  const std::expected<makebelieve::EntryInfo, std::error_code> result =
      tree().status(c.input);

  if (c.expected == Kind::Error) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), std::errc::no_such_file_or_directory);
    return;
  }

  ASSERT_TRUE(result.has_value()) << result.error().message();
  if (c.expected == Kind::Directory) {
    expect_directory(*result);
  } else {
    expect_file(*result, c.size);
  }
}

INSTANTIATE_TEST_SUITE_P(
    InMemoryDirectoryTree,
    StatusEntry,
    ::testing::Values(
        StatusCase{"empty_is_the_root", "", Kind::Directory, 0},
        StatusCase{"dot_is_the_root", ".", Kind::Directory, 0},
        StatusCase{"up_from_a_directory", "sub/..", Kind::Directory, 0},
        StatusCase{"up_from_a_missing_directory", "missing/..", Kind::Directory,
                   0},
        StatusCase{"file", "hello.txt", Kind::File, 11},
        StatusCase{"empty_file", "empty.txt", Kind::File, 0},
        StatusCase{"binary_file", "binary.bin", Kind::File, 4},
        StatusCase{"directory", "sub", Kind::Directory, 0},
        StatusCase{"nested_directory", "sub/deeper", Kind::Directory, 0},
        StatusCase{"nested_file", "sub/nested.txt", Kind::File, 6},
        StatusCase{"leading_dot", "./hello.txt", Kind::File, 11},
        StatusCase{"round_trip", "sub/../hello.txt", Kind::File, 11},
        // Accepted even though `missing` does not exist - the guard is
        // purely lexical and never consults m_entries.
        StatusCase{"round_trip_through_nothing", "missing/../hello.txt",
                   Kind::File, 11},
        StatusCase{"missing", "missing.txt", Kind::Error, 0},
        StatusCase{"missing_under_a_directory", "sub/missing.txt", Kind::Error,
                   0}),
    [](const ::testing::TestParamInfo<StatusCase>& info) {
      return std::string(info.param.label);
    });

// ---------------------------------------------------------------------------
// ls()
// ---------------------------------------------------------------------------

struct LsCase {
  std::string_view label;
  std::filesystem::path input;
  // nullopt means the call is expected to fail.
  std::optional<std::vector<std::string>> expected;
};

class LsListing : public InMemoryDirectoryTree,
                  public ::testing::WithParamInterface<LsCase> {};

TEST_P(LsListing, MatchesExpectation) {
  const LsCase& c = GetParam();
  const std::expected<std::vector<makebelieve::TreeEntry>, std::error_code>
      result = tree().ls(c.input);

  if (!c.expected.has_value()) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), std::errc::no_such_file_or_directory);
    return;
  }

  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(sorted_names(*result), *c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    InMemoryDirectoryTree,
    LsListing,
    ::testing::Values(LsCase{"root", "",
                             std::vector<std::string>{"binary.bin", "empty.txt",
                                                      "hello.txt", "sub"}},
                      LsCase{"dot", ".",
                             std::vector<std::string>{"binary.bin", "empty.txt",
                                                      "hello.txt", "sub"}},
                      LsCase{"subdirectory", "sub",
                             std::vector<std::string>{"deeper", "nested.txt"}},
                      LsCase{"round_trip_through_nothing", "missing/../sub",
                             std::vector<std::string>{"deeper", "nested.txt"}},
                      // An existing but empty directory succeeds with no
                      // entries, which must stay distinguishable from an error.
                      LsCase{"empty_directory", "sub/deeper",
                             std::vector<std::string>{}},
                      LsCase{"file", "hello.txt", std::nullopt},
                      LsCase{"nested_file", "sub/nested.txt", std::nullopt},
                      LsCase{"missing", "missing", std::nullopt}),
    [](const ::testing::TestParamInfo<LsCase>& info) {
      return std::string(info.param.label);
    });

// ---------------------------------------------------------------------------
// read()
// ---------------------------------------------------------------------------

struct ReadCase {
  std::string_view label;
  std::filesystem::path input;
  makebelieve::Offset offset;
  std::size_t size;
  std::string expected;
};

class ReadSlice : public InMemoryDirectoryTree,
                  public ::testing::WithParamInterface<ReadCase> {};

TEST_P(ReadSlice, ReturnsExpectedBytes) {
  const ReadCase& c = GetParam();
  const std::expected<std::string, std::error_code> result =
      tree().read(c.input, c.offset, c.size);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(*result, c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    InMemoryDirectoryTree,
    ReadSlice,
    ::testing::Values(
        ReadCase{"whole_file", "hello.txt", 0, 11, "hello world"},
        ReadCase{"prefix", "hello.txt", 0, 5, "hello"},
        ReadCase{"offset", "hello.txt", 6, 5, "world"},
        ReadCase{"over_read", "hello.txt", 0, 64, "hello world"},
        ReadCase{"offset_and_over_read", "hello.txt", 6, 64, "world"},
        ReadCase{"offset_at_end", "hello.txt", 11, 8, ""},
        ReadCase{"offset_past_end", "hello.txt", 64, 8, ""},
        ReadCase{"zero_size", "hello.txt", 3, 0, ""},
        ReadCase{"empty_file", "empty.txt", 0, 8, ""},
        ReadCase{"binary", "binary.bin", 0, 4,
                 std::string{'\r', '\n', '\0', static_cast<char>(0xff)}},
        ReadCase{"binary_offset", "binary.bin", 2, 2,
                 std::string{'\0', static_cast<char>(0xff)}},
        ReadCase{"nested", "sub/nested.txt", 0, 6, "nested"},
        ReadCase{"round_trip_through_nothing", "missing/../hello.txt", 0, 5,
                 "hello"},
        // A zero-size read succeeds even against a missing file, per the
        // interface contract: it returns before the entry is looked up.
        ReadCase{"zero_size_of_a_missing_file", "missing.txt", 0, 0, ""}),
    [](const ::testing::TestParamInfo<ReadCase>& info) {
      return std::string(info.param.label);
    });

struct ReadFailureCase {
  std::string_view label;
  std::filesystem::path input;
  makebelieve::Offset offset;
  std::size_t size;
  std::errc expected;
};

class ReadFailure : public InMemoryDirectoryTree,
                    public ::testing::WithParamInterface<ReadFailureCase> {};

TEST_P(ReadFailure, FailsAsExpected) {
  const ReadFailureCase& c = GetParam();
  const std::expected<std::string, std::error_code> result =
      tree().read(c.input, c.offset, c.size);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    InMemoryDirectoryTree,
    ReadFailure,
    ::testing::Values(
        ReadFailureCase{"missing", "missing.txt", 0, 8,
                        std::errc::no_such_file_or_directory},
        ReadFailureCase{"missing_under_a_directory", "sub/missing.txt", 0, 8,
                        std::errc::no_such_file_or_directory},
        ReadFailureCase{"missing_interior_directory", "missing/nested.txt", 0,
                        8, std::errc::no_such_file_or_directory},
        ReadFailureCase{"negative_offset", "hello.txt", -1, 8,
                        std::errc::invalid_argument},
        ReadFailureCase{"negative_offset_of_a_missing_file", "missing.txt", -1,
                        8, std::errc::invalid_argument},
        ReadFailureCase{"negative_offset_and_zero_size", "hello.txt", -1, 0,
                        std::errc::invalid_argument},
        // ...and that the escape check runs before even that.
        ReadFailureCase{"escape_beats_negative_offset", "..", -1, 8,
                        std::errc::no_such_file_or_directory},
        ReadFailureCase{"directory", "sub", 0, 8, std::errc::is_a_directory},
        ReadFailureCase{"root", "", 0, 8, std::errc::is_a_directory}),
    [](const ::testing::TestParamInfo<ReadFailureCase>& info) {
      return std::string(info.param.label);
    });

// ---------------------------------------------------------------------------
// Manipulators and the diffs they produce.
// ---------------------------------------------------------------------------

// Subscribes for the fixture's lifetime and records every diff delivered, in
// order, so a test can assert on both the tree's new state and exactly what
// was reported about the change that produced it.
class InMemoryDirectoryTreeChanges : public InMemoryDirectoryTree {
 protected:
  void SetUp() override {
    InMemoryDirectoryTree::SetUp();
    m_subscription = tree().subscribe_to_changes(
        [this](const makebelieve::DirectoryTreeDiff& diff) {
          m_diffs.push_back(diff);
        });
  }

  const std::vector<makebelieve::DirectoryTreeDiff>& diffs() const {
    return m_diffs;
  }

 private:
  std::vector<makebelieve::DirectoryTreeDiff> m_diffs;
  std::optional<makebelieve::Subscription> m_subscription;
};

TEST_F(InMemoryDirectoryTreeChanges, WriteFileCreatesANewFile) {
  tree().write_file("new.txt", "content");

  const std::expected<makebelieve::EntryInfo, std::error_code> status =
      tree().status("new.txt");
  ASSERT_TRUE(status.has_value());
  expect_file(*status, 7);
  EXPECT_EQ(tree().read("new.txt", 0, 7), "content");

  ASSERT_EQ(diffs().size(), 1u);
  EXPECT_FALSE(diffs()[0].everything_dirty);
  EXPECT_EQ(sorted_paths(diffs()[0].entries_changed),
            std::vector<std::string>{"new.txt"});
  EXPECT_EQ(sorted_paths(diffs()[0].child_lists_changed),
            std::vector<std::string>{""});
}

TEST_F(InMemoryDirectoryTreeChanges, WriteFileOverwritesAnExistingFile) {
  tree().write_file("hello.txt", "goodbye");

  EXPECT_EQ(tree().read("hello.txt", 0, 7), "goodbye");

  // The parent's child list did not change - only the file's own content.
  ASSERT_EQ(diffs().size(), 1u);
  EXPECT_EQ(sorted_paths(diffs()[0].entries_changed),
            std::vector<std::string>{"hello.txt"});
  EXPECT_TRUE(diffs()[0].child_lists_changed.empty());
}

TEST_F(InMemoryDirectoryTreeChanges, MakeDirectoryAddsAnEmptyDirectory) {
  tree().make_directory("sub/new_dir");

  const std::expected<std::vector<makebelieve::TreeEntry>, std::error_code>
      listing = tree().ls("sub/new_dir");
  ASSERT_TRUE(listing.has_value());
  EXPECT_TRUE(listing->empty());

  ASSERT_EQ(diffs().size(), 1u);
  EXPECT_EQ(sorted_paths(diffs()[0].entries_changed),
            std::vector<std::string>{"sub/new_dir"});
  EXPECT_EQ(sorted_paths(diffs()[0].child_lists_changed),
            std::vector<std::string>{"sub"});
}

TEST_F(InMemoryDirectoryTreeChanges, RemoveDeletesAFile) {
  tree().remove("hello.txt");

  EXPECT_EQ(tree().status("hello.txt").error(),
            std::errc::no_such_file_or_directory);

  ASSERT_EQ(diffs().size(), 1u);
  EXPECT_EQ(sorted_paths(diffs()[0].entries_changed),
            std::vector<std::string>{"hello.txt"});
  EXPECT_EQ(sorted_paths(diffs()[0].child_lists_changed),
            std::vector<std::string>{""});
}

TEST_F(InMemoryDirectoryTreeChanges, RemoveDeletesADirectoryAndItsContents) {
  tree().remove("sub");

  EXPECT_EQ(tree().status("sub").error(), std::errc::no_such_file_or_directory);
  EXPECT_EQ(tree().status("sub/nested.txt").error(),
            std::errc::no_such_file_or_directory);
  EXPECT_EQ(tree().status("sub/deeper").error(),
            std::errc::no_such_file_or_directory);

  ASSERT_EQ(diffs().size(), 1u);
  EXPECT_EQ(sorted_paths(diffs()[0].entries_changed),
            (std::vector<std::string>{"sub", "sub/deeper", "sub/nested.txt"}));
  EXPECT_EQ(sorted_paths(diffs()[0].child_lists_changed),
            std::vector<std::string>{""});
}

TEST_F(InMemoryDirectoryTreeChanges, SetMtimeUpdatesOnlyTheMtime) {
  const auto original =
      std::get<makebelieve::FileInfo>(*tree().status("hello.txt")).mtime;
  const auto updated = original + std::chrono::hours(1);

  tree().set_mtime("hello.txt", updated);

  const std::expected<makebelieve::EntryInfo, std::error_code> status =
      tree().status("hello.txt");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(std::get<makebelieve::FileInfo>(*status).mtime, updated);
  EXPECT_EQ(tree().read("hello.txt", 0, 11), "hello world");

  ASSERT_EQ(diffs().size(), 1u);
  EXPECT_EQ(sorted_paths(diffs()[0].entries_changed),
            std::vector<std::string>{"hello.txt"});
  EXPECT_TRUE(diffs()[0].child_lists_changed.empty());
}

TEST_F(InMemoryDirectoryTreeChanges, SetMtimeCanTargetTheRoot) {
  const auto updated = std::chrono::file_clock::now() + std::chrono::hours(1);
  tree().set_mtime("", updated);

  const std::expected<makebelieve::EntryInfo, std::error_code> status =
      tree().status("");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(std::get<makebelieve::DirectoryInfo>(*status).mtime, updated);

  ASSERT_EQ(diffs().size(), 1u);
  EXPECT_EQ(diffs()[0].entries_changed, std::vector<std::filesystem::path>{""});
}

TEST_F(InMemoryDirectoryTreeChanges, EveryChangeIsReportedInOrder) {
  tree().write_file("a.txt", "1");
  tree().write_file("b.txt", "2");
  tree().remove("a.txt");

  ASSERT_EQ(diffs().size(), 3u);
  EXPECT_EQ(diffs()[0].entries_changed,
            std::vector<std::filesystem::path>{"a.txt"});
  EXPECT_EQ(diffs()[1].entries_changed,
            std::vector<std::filesystem::path>{"b.txt"});
  EXPECT_EQ(diffs()[2].entries_changed,
            std::vector<std::filesystem::path>{"a.txt"});
}

TEST_F(InMemoryDirectoryTree, MultipleSubscribersAreAllNotified) {
  int first_calls = 0;
  int second_calls = 0;
  const makebelieve::Subscription first =
      tree().subscribe_to_changes([&](const auto&) { ++first_calls; });
  const makebelieve::Subscription second =
      tree().subscribe_to_changes([&](const auto&) { ++second_calls; });

  tree().write_file("new.txt", "x");

  EXPECT_EQ(first_calls, 1);
  EXPECT_EQ(second_calls, 1);
}

TEST_F(InMemoryDirectoryTree, UnsubscribingStopsFurtherNotifications) {
  int calls = 0;
  {
    const makebelieve::Subscription subscription =
        tree().subscribe_to_changes([&](const auto&) { ++calls; });
    tree().write_file("new.txt", "x");
  }
  EXPECT_EQ(calls, 1);

  tree().write_file("another.txt", "y");
  EXPECT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// Thread safety.
// ---------------------------------------------------------------------------

// A callback may read the tree: notifications fire with the write's lock
// released, so the re-entrant read takes a fresh shared lock instead of
// deadlocking, and sees the just-written value.
TEST_F(InMemoryDirectoryTree, ACallbackCanReadTheTreeDuringNotification) {
  std::optional<std::string> observed;
  const makebelieve::Subscription subscription =
      tree().subscribe_to_changes([&](const auto&) {
        const std::expected<std::string, std::error_code> content =
            tree().read("reentrant.txt", 0, 64);
        if (content.has_value()) {
          observed = *content;
        }
      });

  tree().write_file("reentrant.txt", "written");

  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(*observed, "written");
}

// Many threads at once: each writer owns distinct files while readers race
// them. Every write must read back afterwards as its own name, with no data
// race.
TEST_F(InMemoryDirectoryTree, ConcurrentReadersAndWritersStayConsistent) {
  constexpr int k_writers = 8;
  constexpr int k_writes_per_thread = 100;

  const auto name_for = [](int writer, int index) {
    return "w" + std::to_string(writer) + "_" + std::to_string(index) + ".txt";
  };

  {
    std::vector<std::jthread> threads;
    threads.reserve(k_writers * 2);
    for (int w = 0; w < k_writers; ++w) {
      threads.emplace_back([this, w, &name_for] {
        for (int i = 0; i < k_writes_per_thread; ++i) {
          const std::string name = name_for(w, i);
          tree().write_file(name, name);
        }
      });
    }
    // Readers race the writers; ls() is an O(n) scan, so it runs only
    // occasionally, keeping the readers contention pressure rather than a hot
    // loop.
    for (int r = 0; r < k_writers; ++r) {
      threads.emplace_back([this] {
        for (int i = 0; i < k_writes_per_thread; ++i) {
          (void)tree().status("w0_0.txt");
          if (i % 20 == 0) {
            (void)tree().ls("");
          }
        }
      });
    }
  }  // jthreads join here

  // Every write landed and reads back as itself.
  for (int w = 0; w < k_writers; ++w) {
    for (int i = 0; i < k_writes_per_thread; ++i) {
      const std::string name = name_for(w, i);
      const std::expected<std::string, std::error_code> content =
          tree().read(name, 0, 64);
      ASSERT_TRUE(content.has_value()) << name;
      EXPECT_EQ(*content, name);
    }
  }
}

}  // namespace
