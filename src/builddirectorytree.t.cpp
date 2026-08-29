// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "inmemorydirectorytree.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace makebelieve;

// A runner that produces fixed bytes for any command, so the tree's behaviour
// can be tested without a shell. Written with the completion handler spelled as
// its concrete type.
BuildDirectoryTree::CommandRunner returning(std::string output) {
  return [output = std::move(output)](
             std::string /*command*/, std::stop_token /*stop*/,
             BuildDirectoryTree::BuildComplete on_done) { on_done(output); };
}

class BuildDirectoryTreeTest : public ::testing::Test {
 protected:
  InMemoryDirectoryTree source;

  // A real, throwaway directory for the tests that drive the real shell runner.
  std::filesystem::path work =
      std::filesystem::temp_directory_path() /
      ("makebelieve-test-" +
       std::string(
           ::testing::UnitTest::GetInstance()->current_test_info()->name()));

  void SetUp() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
  }

  void write_manifest(std::string_view text) {
    source.write_file("build.makebelieve", text);
  }

  void write_input(std::string_view name, std::string_view content) {
    std::ofstream stream(work / name, std::ios::binary);
    stream << content;
  }

  static std::size_t output_size(const BuildDirectoryTree& tree,
                                 const std::filesystem::path& path) {
    const auto info = tree.status(path);
    EXPECT_TRUE(info.has_value());
    return std::get<FileInfo>(*info).size;
  }

  // Reads a whole output. The read itself is what triggers the lazy build, so
  // it deliberately does not size the request from a prior status() call - that
  // would still report the pre-build placeholder size.
  static std::string read_output(const BuildDirectoryTree& tree,
                                 const std::filesystem::path& path) {
    const auto content = tree.read(path, 0, 1U << 20);
    EXPECT_TRUE(content.has_value());
    return *content;
  }
};

TEST_F(BuildDirectoryTreeTest, NoManifestYieldsAnEmptyTree) {
  int runs = 0;
  const BuildDirectoryTree tree(
      source, [&runs](std::string, std::stop_token,
                      BuildDirectoryTree::BuildComplete done) {
        ++runs;
        done(std::string{});
      });

  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  EXPECT_TRUE(root->empty());
  EXPECT_EQ(runs, 0);
}

TEST_F(BuildDirectoryTreeTest,
       DeclaredOutputsAppearAsPlaceholdersBeforeAnyRead) {
  write_manifest("@/output.txt <- anything %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(
      source, [&runs](std::string, std::stop_token,
                      BuildDirectoryTree::BuildComplete done) {
        ++runs;
        done(std::string("built"));
      });

  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  ASSERT_EQ(root->size(), 1U);
  EXPECT_EQ((*root)[0].name, "output.txt");
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);  // one null byte
  EXPECT_EQ(runs, 0);                              // not built until read
}

TEST_F(BuildDirectoryTreeTest, ReadHandsTheRawCommandToTheRunner) {
  write_manifest("@/output.txt <- cp input.txt %out\n");

  std::vector<std::string> commands;
  const BuildDirectoryTree tree(
      source, [&commands](std::string command, std::stop_token,
                          BuildDirectoryTree::BuildComplete done) {
        commands.push_back(std::move(command));
        done(std::string("result"));
      });

  EXPECT_EQ(read_output(tree, "output.txt"), "result");
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0], "cp input.txt %out");  // %out is the runner's concern
}

TEST_F(BuildDirectoryTreeTest, BuildsLazilyAndOncePerOutputOnSuccess) {
  write_manifest("@/output.txt <- build %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(
      source, [&runs](std::string, std::stop_token,
                      BuildDirectoryTree::BuildComplete done) {
        ++runs;
        done(std::string("hello world"));
      });

  EXPECT_EQ(runs, 0);
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
  EXPECT_EQ(output_size(tree, "output.txt"), 11U);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
  EXPECT_EQ(runs, 1);  // not rebuilt
}

TEST_F(BuildDirectoryTreeTest, RetriesAfterAFailedBuild) {
  write_manifest("@/output.txt <- build %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(
      source, [&runs](std::string, std::stop_token,
                      BuildDirectoryTree::BuildComplete done) {
        ++runs;
        if (runs == 1) {
          done(std::unexpected(std::make_error_code(std::errc::io_error)));
        } else {
          done(std::string("recovered"));
        }
      });

  // The failed first build leaves the placeholder in place...
  read_output(tree, "output.txt");
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);

  // ...so a later read tries again, and this time it sticks.
  EXPECT_EQ(read_output(tree, "output.txt"), "recovered");
  EXPECT_EQ(runs, 2);
  EXPECT_EQ(read_output(tree, "output.txt"), "recovered");
  EXPECT_EQ(runs, 2);
}

TEST_F(BuildDirectoryTreeTest, OnlyGeneratedOutputsAreVisible) {
  // The source files - the manifest included - are not part of the output
  // namespace; only the declared `@/` outputs are.
  source.write_file("input.txt", "some input");
  write_manifest("@/output.txt <- build %out\n");

  const BuildDirectoryTree tree(source, returning("built"));

  const auto root = tree.ls("");
  ASSERT_TRUE(root.has_value());
  ASSERT_EQ(root->size(), 1U);
  EXPECT_EQ((*root)[0].name, "output.txt");
  EXPECT_FALSE(tree.status("input.txt").has_value());
  EXPECT_FALSE(tree.status("build.makebelieve").has_value());
}

TEST_F(BuildDirectoryTreeTest, CreatesParentDirectoriesForNestedOutputs) {
  write_manifest("@/out/deep/file.txt <- build %out\n");

  const BuildDirectoryTree tree(source, returning("nested"));

  const auto out = tree.status("out");
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(std::holds_alternative<DirectoryInfo>(*out));
  EXPECT_EQ(read_output(tree, "out/deep/file.txt"), "nested");
}

// The completion handler can be taken as `auto`, so a runner never has to name
// BuildComplete.
TEST_F(BuildDirectoryTreeTest, RunnerCompletionHandlerCanBeGeneric) {
  write_manifest("@/output.txt <- build %out\n");

  const BuildDirectoryTree tree(source,
                                [](std::string, std::stop_token, auto done) {
                                  done(std::string("generic"));
                                });

  EXPECT_EQ(read_output(tree, "output.txt"), "generic");
}

// An asynchronous runner reports later: the triggering read sees the
// placeholder, and the real content only appears once the runner completes.
TEST_F(BuildDirectoryTreeTest, AsynchronousRunnerCompletesLater) {
  write_manifest("@/output.txt <- build %out\n");

  std::vector<BuildDirectoryTree::BuildComplete> deferred;
  const BuildDirectoryTree tree(
      source, [&deferred](std::string, std::stop_token,
                          BuildDirectoryTree::BuildComplete done) {
        deferred.push_back(std::move(done));
      });

  // The first read starts the build but it has not reported back yet.
  EXPECT_EQ(read_output(tree, "output.txt"), std::string(1, '\0'));
  EXPECT_EQ(output_size(tree, "output.txt"), 1U);
  ASSERT_EQ(deferred.size(), 1U);

  // A second read must not start a second build while the first is in flight.
  EXPECT_EQ(read_output(tree, "output.txt"), std::string(1, '\0'));
  ASSERT_EQ(deferred.size(), 1U);

  // Completing it swaps in the real content.
  deferred.front()(std::string("done later"));
  EXPECT_EQ(read_output(tree, "output.txt"), "done later");
  EXPECT_EQ(output_size(tree, "output.txt"), 10U);
}

// Destroying the tree requests a stop on the token handed to in-flight runners.
TEST_F(BuildDirectoryTreeTest, DestructionRequestsStop) {
  write_manifest("@/output.txt <- build %out\n");

  std::stop_token token;
  {
    const BuildDirectoryTree tree(
        source, [&token](std::string, std::stop_token stop,
                         BuildDirectoryTree::BuildComplete) {
          token = std::move(stop);  // never completes: an abandoned build
        });
    EXPECT_EQ(read_output(tree, "output.txt"), std::string(1, '\0'));
    EXPECT_TRUE(token.stop_possible());
    EXPECT_FALSE(token.stop_requested());
  }

  EXPECT_TRUE(token.stop_requested());
}

// The bundled shell runner actually drives the system shell end to end.
// `cmake -E copy` runs the same under either shell, and cmake is on PATH
// wherever ctest is.
TEST_F(BuildDirectoryTreeTest, ShellRunnerBuildsLazilyThroughTheShell) {
  write_input("input.txt", "hello world");
  write_manifest("@/output.txt <- cmake -E copy input.txt %out\n");

  const BuildDirectoryTree tree(source, BuildDirectoryTree::shell_runner(work));

  EXPECT_EQ(output_size(tree, "output.txt"), 1U);  // placeholder before read
  EXPECT_EQ(read_output(tree, "output.txt"), "hello world");
  EXPECT_EQ(output_size(tree, "output.txt"), 11U);
}

}  // namespace
