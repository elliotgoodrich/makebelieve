// SPDX-License-Identifier: MIT
#include "realdirectorytree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

std::filesystem::path platform_absolute_path() {
#ifdef _WIN32
  return std::filesystem::path("C:\\Windows");
#else
  return std::filesystem::path("/etc");
#endif
}

// Binary content built byte by byte rather than from a literal: the embedded
// nul and the 0xff both need to survive, and 0xff is not portably a char.
std::string binary_contents() {
  return std::string{'\r', '\n', '\0', static_cast<char>(0xff)};
}

// Returns an empty path on failure so the caller can ASSERT - create_directory
// returning false means the name already existed, which is the collision we
// must not silently share with another run.
std::filesystem::path make_temp_directory() {
  std::random_device entropy;
  for (int attempt = 0; attempt < 5; ++attempt) {
    const std::filesystem::path candidate =
        std::filesystem::temp_directory_path() /
        ("makebelieve_rdt_" + std::to_string(entropy()) + "_" +
         std::to_string(entropy()));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error) && !error) {
      return candidate;
    }
  }
  return {};
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  out.close();
  ASSERT_TRUE(out) << "could not write " << path.string();
}

void expect_file(const makebelieve::EntryInfo& info, std::size_t expected_size) {
  ASSERT_TRUE(std::holds_alternative<makebelieve::FileInfo>(info))
      << "expected a file, got a directory";
  EXPECT_EQ(std::get<makebelieve::FileInfo>(info).size, expected_size);
}

void expect_directory(const makebelieve::EntryInfo& info) {
  EXPECT_TRUE(std::holds_alternative<makebelieve::DirectoryInfo>(info))
      << "expected a directory, got a file";
}

// ls() order is unspecified on both platforms, so every listing assertion goes
// through here rather than indexing the raw vector.
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

// Named for the type under test so TEST_F reads as RealDirectoryTree.<case>.
// That takes the name, so the class under test is spelled makebelieve::
// throughout this file.
class RealDirectoryTree : public ::testing::Test {
protected:
  void SetUp() override {
    m_root = make_temp_directory();
    ASSERT_FALSE(m_root.empty()) << "could not create a temp directory";

    write_file(m_root / "hello.txt", "hello world");
    write_file(m_root / "empty.txt", "");
    write_file(m_root / "binary.bin", binary_contents());
    ASSERT_TRUE(std::filesystem::create_directory(m_root / "sub"));
    write_file(m_root / "sub" / "nested.txt", "nested");
    ASSERT_TRUE(std::filesystem::create_directory(m_root / "sub" / "deeper"));

    m_tree.emplace(m_root);
  }

  void TearDown() override {
    m_tree.reset();
    std::error_code ignored;
    std::filesystem::remove_all(m_root, ignored);
  }

  const makebelieve::RealDirectoryTree& tree() const { return *m_tree; }
  const std::filesystem::path& root() const { return m_root; }

private:
  std::filesystem::path m_root;
  // Neither copyable nor movable, so it is emplaced once the tree is on disk.
  std::optional<makebelieve::RealDirectoryTree> m_tree;
};

// ---------------------------------------------------------------------------
// Paths that escape the root. status() and ls() collapse every failure into one
// code, so one table can assert the same expectation across all four calls.
// ---------------------------------------------------------------------------

struct EscapeCase {
  std::string_view label;
  std::filesystem::path input;
};

class EscapingPath : public RealDirectoryTree,
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

  // Check 0-size read (that short circuits) still returns an error
  const std::expected<std::string, std::error_code> empty_read =
      tree().read(c.input, 0, 0);
  ASSERT_FALSE(empty_read.has_value());
  EXPECT_EQ(empty_read.error(), std::errc::no_such_file_or_directory);
}

INSTANTIATE_TEST_SUITE_P(
    RealDirectoryTree, EscapingPath,
    ::testing::Values(
        EscapeCase{"parent", ".."},
        EscapeCase{"parent_file", "../outside.txt"},
        EscapeCase{"through_a_real_directory", "sub/../../outside.txt"},
        EscapeCase{"interior_segments_collapse", "sub/../.."},
        EscapeCase{"root_directory", "/absolute.txt"},
        EscapeCase{"absolute", platform_absolute_path()}),
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

class StatusEntry : public RealDirectoryTree,
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
    RealDirectoryTree, StatusEntry,
    ::testing::Values(
        StatusCase{"empty_is_the_root", "", Kind::Directory, 0},
        StatusCase{"dot_is_the_root", ".", Kind::Directory, 0},
        StatusCase{"up_from_a_real_directory", "sub/..", Kind::Directory, 0},
        StatusCase{"up_from_a_missing_directory", "missing/..", Kind::Directory,
                   0},
        // Files report their size in bytes, whatever the content.
        StatusCase{"file", "hello.txt", Kind::File, 11},
        StatusCase{"empty_file", "empty.txt", Kind::File, 0},
        StatusCase{"binary_file", "binary.bin", Kind::File, 4},
        // Directories report DirectoryInfo, which carries no size.
        StatusCase{"directory", "sub", Kind::Directory, 0},
        StatusCase{"nested_directory", "sub/deeper", Kind::Directory, 0},
        StatusCase{"nested_file", "sub/nested.txt", Kind::File, 6},
        // Normalisation that lands back on a real file.
        StatusCase{"leading_dot", "./hello.txt", Kind::File, 11},
        StatusCase{"round_trip", "sub/../hello.txt", Kind::File, 11},
        // Accepted even though `missing` does not exist - the guard is purely
        // lexical and never consults the disk.
        StatusCase{"round_trip_through_nothing", "missing/../hello.txt",
                   Kind::File, 11},
        // Every failure collapses to one code, whatever the OS reported.
        StatusCase{"missing", "missing.txt", Kind::Error, 0},
        StatusCase{"missing_under_a_directory", "sub/missing.txt", Kind::Error,
                   0},
        StatusCase{"file_used_as_a_directory", "hello.txt/nested", Kind::Error,
                   0}),
    [](const ::testing::TestParamInfo<StatusCase>& info) {
      return std::string(info.param.label);
    });

TEST_F(RealDirectoryTree, MTimeMatchesTheFileSystem) {
  // Both alternatives carry an mtime, and the Windows backend fills them from
  // different structs, so one file and one directory cover both branches.
  for (const std::string_view relative : {"hello.txt", "sub"}) {
    const std::expected<makebelieve::EntryInfo, std::error_code> result =
        tree().status(std::filesystem::path(relative));
    ASSERT_TRUE(result.has_value()) << relative << ": "
                                    << result.error().message();

    const std::chrono::file_clock::time_point reported =
        std::visit([](const auto& info) { return info.mtime; }, *result);
    std::error_code error;
    const std::filesystem::file_time_type on_disk =
        std::filesystem::last_write_time(root() / relative, error);
    ASSERT_FALSE(error) << error.message();

    EXPECT_LT(std::chrono::abs(reported - on_disk), std::chrono::seconds(2))
        << relative;
  }
}

// ---------------------------------------------------------------------------
// ls()
// ---------------------------------------------------------------------------

struct LsCase {
  std::string_view label;
  std::filesystem::path input;
  // nullopt means the call is expected to fail.
  std::optional<std::vector<std::string>> expected;
};

class LsListing : public RealDirectoryTree,
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
    RealDirectoryTree, LsListing,
    ::testing::Values(
        LsCase{"root", "",
               std::vector<std::string>{"binary.bin", "empty.txt", "hello.txt",
                                        "sub"}},
        LsCase{"dot", ".",
               std::vector<std::string>{"binary.bin", "empty.txt", "hello.txt",
                                        "sub"}},
        LsCase{"subdirectory", "sub",
               std::vector<std::string>{"deeper", "nested.txt"}},
        LsCase{"round_trip_through_nothing", "missing/../sub",
               std::vector<std::string>{"deeper", "nested.txt"}},
        // An existing but empty directory succeeds with no entries, which the
        // contract calls out and which must stay distinguishable from an error.
        LsCase{"empty_directory", "sub/deeper", std::vector<std::string>{}},
        // Listing a file is an error, as is listing what is not there.
        LsCase{"file", "hello.txt", std::nullopt},
        LsCase{"nested_file", "sub/nested.txt", std::nullopt},
        LsCase{"missing", "missing", std::nullopt}),
    [](const ::testing::TestParamInfo<LsCase>& info) {
      return std::string(info.param.label);
    });

TEST_F(RealDirectoryTree, LsAgreesWithStatusForEveryChild) {
  const std::expected<std::vector<makebelieve::TreeEntry>, std::error_code>
      listing = tree().ls("");
  ASSERT_TRUE(listing.has_value()) << listing.error().message();
  ASSERT_EQ(listing->size(), 4u);

  for (const makebelieve::TreeEntry& entry : *listing) {
    const std::expected<makebelieve::EntryInfo, std::error_code> direct =
        tree().status(entry.name);
    ASSERT_TRUE(direct.has_value())
        << entry.name.string() << ": " << direct.error().message();
    ASSERT_EQ(entry.info.index(), direct->index()) << entry.name.string();

    if (const auto* file = std::get_if<makebelieve::FileInfo>(&entry.info)) {
      EXPECT_EQ(file->size, std::get<makebelieve::FileInfo>(*direct).size)
          << entry.name.string();
    }
  }
}

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

class ReadSlice : public RealDirectoryTree,
                  public ::testing::WithParamInterface<ReadCase> {};

TEST_P(ReadSlice, ReturnsExpectedBytes) {
  const ReadCase& c = GetParam();
  const std::expected<std::string, std::error_code> result =
      tree().read(c.input, c.offset, c.size);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(*result, c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    RealDirectoryTree, ReadSlice,
    ::testing::Values(
        // Whole file, a prefix, and an interior slice.
        ReadCase{"whole_file", "hello.txt", 0, 11, "hello world"},
        ReadCase{"prefix", "hello.txt", 0, 5, "hello"},
        ReadCase{"offset", "hello.txt", 6, 5, "world"},
        // Asking for more than there is okay.
        ReadCase{"over_read", "hello.txt", 0, 64, "hello world"},
        ReadCase{"offset_and_over_read", "hello.txt", 6, 64, "world"},
        ReadCase{"offset_at_end", "hello.txt", 11, 8, ""},
        ReadCase{"offset_past_end", "hello.txt", 64, 8, ""},
        // Zero bytes asked for.
        ReadCase{"zero_size", "hello.txt", 3, 0, ""},
        ReadCase{"empty_file", "empty.txt", 0, 8, ""},
        // Bytes pass through untranslated: \r\n survives, so does an embedded
        // nul, so does 0xff.
        ReadCase{"binary", "binary.bin", 0, 4,
                 std::string{'\r', '\n', '\0', static_cast<char>(0xff)}},
        ReadCase{"binary_offset", "binary.bin", 2, 2,
                 std::string{'\0', static_cast<char>(0xff)}},
        ReadCase{"nested", "sub/nested.txt", 0, 6, "nested"},
        ReadCase{"round_trip_through_nothing", "missing/../hello.txt", 0, 5,
                 "hello"},
        // A zero-size read returns before the file is opened
        ReadCase{"zero_size_of_a_missing_file", "missing.txt", 0, 0, ""}),
    [](const ::testing::TestParamInfo<ReadCase>& info) {
      return std::string(info.param.label);
    });

struct ReadFailureCase {
  std::string_view label;
  std::filesystem::path input;
  makebelieve::Offset offset;
  std::size_t size;
  // nullopt where the platforms report different conditions.
  std::optional<std::errc> expected;
};

class ReadFailure : public RealDirectoryTree,
                    public ::testing::WithParamInterface<ReadFailureCase> {};

TEST_P(ReadFailure, FailsAsExpected) {
  const ReadFailureCase& c = GetParam();
  const std::expected<std::string, std::error_code> result =
      tree().read(c.input, c.offset, c.size);

  ASSERT_FALSE(result.has_value());
  if (c.expected.has_value()) {
    EXPECT_EQ(result.error(), *c.expected);
  } else {
    EXPECT_TRUE(static_cast<bool>(result.error()))
        << "expected a real error code, got a default-constructed one";
  }
}

INSTANTIATE_TEST_SUITE_P(
    RealDirectoryTree, ReadFailure,
    ::testing::Values(
        // A missing leaf and a missing interior directory report different raw
        // values on Windows, but map to the same condition - which is why these
        // compare conditions and never .value().
        ReadFailureCase{"missing", "missing.txt", 0, 8,
                        std::errc::no_such_file_or_directory},
        ReadFailureCase{"missing_under_a_directory", "sub/missing.txt", 0, 8,
                        std::errc::no_such_file_or_directory},
        ReadFailureCase{"missing_interior_directory", "missing/nested.txt", 0,
                        8, std::errc::no_such_file_or_directory},
        // The negative-offset guard
        ReadFailureCase{"negative_offset", "hello.txt", -1, 8,
                        std::errc::invalid_argument},
        ReadFailureCase{"negative_offset_of_a_missing_file", "missing.txt", -1,
                        8, std::errc::invalid_argument},
        ReadFailureCase{"negative_offset_and_zero_size", "hello.txt", -1, 0,
                        std::errc::invalid_argument},
        // ...and that the escape check runs before even that.
        ReadFailureCase{"escape_beats_negative_offset", "..", -1, 8,
                        std::errc::no_such_file_or_directory},
        // Reading a directory fails on both, but as EISDIR on Linux and
        // ERROR_ACCESS_DENIED on Windows, which are different conditions.
        ReadFailureCase{"directory", "sub", 0, 8, std::nullopt},
        ReadFailureCase{"root", "", 0, 8, std::nullopt},
        // ENOTDIR against ERROR_PATH_NOT_FOUND, likewise.
        ReadFailureCase{"file_used_as_a_directory", "hello.txt/nested", 0, 8,
                        std::nullopt}),
    [](const ::testing::TestParamInfo<ReadFailureCase>& info) {
      return std::string(info.param.label);
    });

class RealDirectoryTreeWithAMissingRoot : public ::testing::Test {
protected:
  void SetUp() override {
    const std::filesystem::path parent = make_temp_directory();
    ASSERT_FALSE(parent.empty()) << "could not create a temp directory";
    m_parent = parent;
    m_root = parent / "never_created";
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(m_parent, ignored);
  }

  const std::filesystem::path& root() const { return m_root; }

private:
  std::filesystem::path m_parent;
  std::filesystem::path m_root;
};

TEST_F(RealDirectoryTreeWithAMissingRoot, ConstructionSucceeds) {
  // weakly_canonical must not throw on a path that is not there, so a
  // misconfigured root surfaces when it is queried rather than at startup.
  EXPECT_NO_THROW({ const makebelieve::RealDirectoryTree tree(root()); });
}

TEST_F(RealDirectoryTreeWithAMissingRoot, EveryOperationFails) {
  const makebelieve::RealDirectoryTree tree(root());

  const std::expected<makebelieve::EntryInfo, std::error_code> status =
      tree.status("");
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error(), std::errc::no_such_file_or_directory);

  const std::expected<std::vector<makebelieve::TreeEntry>, std::error_code>
      listing = tree.ls("");
  ASSERT_FALSE(listing.has_value());
  EXPECT_EQ(listing.error(), std::errc::no_such_file_or_directory);

  const std::expected<std::string, std::error_code> read =
      tree.read("anything.txt", 0, 8);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), std::errc::no_such_file_or_directory);
}

}  // namespace
