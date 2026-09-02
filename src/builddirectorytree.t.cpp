// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "inmemorydirectorytree.hpp"
#include "realdirectorytree.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace makebelieve;

// The tests here care about an output's bytes, not its traced inputs, so wrap a
// plain string as a successful BuildResult carrying no inputs.
BuildDirectoryTree::BuildResult built(std::string bytes) {
  return BuildDirectoryTree::BuildOutput{.bytes = std::move(bytes),
                                         .inputs = {}};
}

// A runner that produces fixed bytes for any command, so the tree's behaviour
// can be tested without a shell. Written with the completion handler spelled as
// its concrete type.
BuildDirectoryTree::CommandRunner returning(std::string output) {
  return [output = std::move(output)](
             std::string /*command*/, std::stop_token /*stop*/,
             BuildDirectoryTree::BuildComplete on_done) {
    on_done(built(output));
  };
}

// A runner that reports a fixed set of traced @a inputs alongside its output,
// and whose output ("build N") reflects how many times it has run, so a rebuild
// is observable. @a runs is bumped on every invocation.
BuildDirectoryTree::CommandRunner counting_with_inputs(
    int& runs,
    std::vector<std::filesystem::path> inputs) {
  return [&runs, inputs = std::move(inputs)](
             std::string /*command*/, std::stop_token /*stop*/,
             BuildDirectoryTree::BuildComplete on_done) {
    ++runs;
    on_done(BuildDirectoryTree::BuildOutput{
        .bytes = "build " + std::to_string(runs), .inputs = inputs});
  };
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
        done(built(std::string{}));
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
        done(built("built"));
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
        done(built("result"));
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
        done(built("hello world"));
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
          done(built("recovered"));
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

  const BuildDirectoryTree tree(
      source,
      [](std::string, std::stop_token, auto done) { done(built("generic")); });

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
  deferred.front()(built("done later"));
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

// ---------------------------------------------------------------------------
// Rebuilding when a traced input changes.
// ---------------------------------------------------------------------------

// Once an output has been read, a change to one of the inputs its build traced
// rebuilds it eagerly - the new content is in place before anyone reads again.
TEST_F(BuildDirectoryTreeTest, RebuildsAReadOutputWhenATracedInputChanges) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt <- build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));

  // The first read builds it lazily and records input.txt as a dependency.
  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");
  EXPECT_EQ(runs, 1);

  // Changing that input rebuilds the output without a new read...
  source.write_file("input.txt", "v2");
  EXPECT_EQ(runs, 2);

  // ...and the fresh content is already in place for the next read.
  EXPECT_EQ(read_output(tree, "output.txt"), "build 2");
  EXPECT_EQ(runs, 2);  // that read did not trigger yet another build
}

// The eager rebuild's new content reaches the tree's own subscribers, so a
// downstream watcher learns the output changed.
TEST_F(BuildDirectoryTreeTest, AnEagerRebuildNotifiesSubscribers) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt <- build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));
  read_output(tree, "output.txt");  // build and materialise it

  std::vector<std::filesystem::path> changed;
  const Subscription subscription =
      tree.subscribe_to_changes([&changed](const DirectoryTreeDiff& diff) {
        changed.insert(changed.end(), diff.entries_changed.begin(),
                       diff.entries_changed.end());
      });

  source.write_file("input.txt", "v2");  // eager rebuild -> notification

  EXPECT_EQ(runs, 2);
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0], std::filesystem::path("output.txt"));
}

// A change to a file the output never read leaves it alone.
TEST_F(BuildDirectoryTreeTest, IgnoresChangesToUntrackedFiles) {
  source.write_file("input.txt", "v1");
  source.write_file("other.txt", "x");
  write_manifest("@/output.txt <- build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));
  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");
  EXPECT_EQ(runs, 1);

  source.write_file("other.txt", "y");  // not a dependency of output.txt
  EXPECT_EQ(runs, 1);                   // so no rebuild
}

// A never-read output is not eagerly built: with nothing having read it, it has
// no recorded dependencies to react to, and stays lazy until first read.
TEST_F(BuildDirectoryTreeTest, DoesNotEagerlyBuildAnUnreadOutput) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt <- build input.txt %out\n");

  int runs = 0;
  const BuildDirectoryTree tree(source,
                                counting_with_inputs(runs, {"input.txt"}));

  source.write_file("input.txt", "v2");
  EXPECT_EQ(runs, 0);  // never read, so never built

  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");  // built on first read
  EXPECT_EQ(runs, 1);
}

// A rebuild replaces the tracked dependency set: an input the command stops
// reading no longer triggers rebuilds, and one it starts reading now does.
TEST_F(BuildDirectoryTreeTest, ARebuildRefreshesTheTrackedInputs) {
  source.write_file("a.txt", "1");
  source.write_file("b.txt", "1");
  write_manifest("@/output.txt <- build %out\n");

  int runs = 0;
  // The first build reads a.txt; every rebuild thereafter reads b.txt instead.
  const BuildDirectoryTree tree(
      source, [&runs](std::string, std::stop_token,
                      BuildDirectoryTree::BuildComplete on_done) {
        ++runs;
        std::vector<std::filesystem::path> inputs =
            runs == 1 ? std::vector<std::filesystem::path>{"a.txt"}
                      : std::vector<std::filesystem::path>{"b.txt"};
        on_done(BuildDirectoryTree::BuildOutput{
            .bytes = "build " + std::to_string(runs),
            .inputs = std::move(inputs)});
      });

  read_output(tree, "output.txt");  // runs=1, depends on a.txt
  EXPECT_EQ(runs, 1);

  source.write_file("a.txt", "2");  // rebuilds; now depends on b.txt
  EXPECT_EQ(runs, 2);

  source.write_file("a.txt", "3");  // no longer a dependency
  EXPECT_EQ(runs, 2);

  source.write_file("b.txt", "2");  // now it is
  EXPECT_EQ(runs, 3);
}

// An input change that lands while a rebuild is in flight does not start a
// second concurrent build; instead the in-flight one, on completing, notices it
// went stale and rebuilds once more - coalescing the changes.
TEST_F(BuildDirectoryTreeTest, CoalescesInputChangesDuringAnInFlightRebuild) {
  source.write_file("input.txt", "v1");
  write_manifest("@/output.txt <- build %out\n");

  std::vector<BuildDirectoryTree::BuildComplete> deferred;
  int runs = 0;
  const BuildDirectoryTree tree(source,
                                [&](std::string, std::stop_token,
                                    BuildDirectoryTree::BuildComplete on_done) {
                                  ++runs;
                                  deferred.push_back(std::move(on_done));
                                });

  const auto complete_latest = [&deferred](std::string bytes) {
    deferred.back()(BuildDirectoryTree::BuildOutput{.bytes = std::move(bytes),
                                                    .inputs = {"input.txt"}});
  };

  // Read starts build #1; complete it so input.txt is recorded as a dependency.
  read_output(tree, "output.txt");
  ASSERT_EQ(deferred.size(), 1U);
  complete_latest("build 1");
  EXPECT_EQ(read_output(tree, "output.txt"), "build 1");
  EXPECT_EQ(runs, 1);

  // An input change starts eager rebuild #2 (in flight, not yet reported).
  source.write_file("input.txt", "v2");
  EXPECT_EQ(runs, 2);
  ASSERT_EQ(deferred.size(), 2U);

  // A second change while #2 is in flight starts no new build yet.
  source.write_file("input.txt", "v3");
  EXPECT_EQ(runs, 2);
  ASSERT_EQ(deferred.size(), 2U);

  // Completing #2 sees the output went stale mid-build and kicks off #3.
  complete_latest("build 2");
  EXPECT_EQ(runs, 3);
  ASSERT_EQ(deferred.size(), 3U);

  // #3 settles it: no further change arrived, so no rebuild #4.
  complete_latest("build 3");
  EXPECT_EQ(read_output(tree, "output.txt"), "build 3");
  EXPECT_EQ(runs, 3);
}

#if defined(_WIN32) || defined(__linux__)
// End to end: a real watched source, real shell builds, and real tracing.
// Editing a traced input on disk rebuilds the output that read it, with no one
// re-reading it. Guarded to the platforms where tracing is wired up.
TEST_F(BuildDirectoryTreeTest, RebuildsThroughRealTracingWhenAnInputChanges) {
  using namespace std::chrono_literals;

  write_input("input.txt", "v1");
  // `cmake -E copy` reads input.txt (which the tracer sees) into %out.
  std::ofstream(work / "build.makebelieve", std::ios::binary)
      << "@/output.txt <- cmake -E copy input.txt %out\n";

  const RealDirectoryTree real_source(work);
  const BuildDirectoryTree tree(real_source,
                                BuildDirectoryTree::shell_runner(work));

  // First read builds it and traces input.txt as a dependency.
  EXPECT_EQ(read_output(tree, "output.txt"), "v1");

  // Change the input on disk; the watcher notices and the output rebuilds. That
  // is asynchronous (it runs on the watcher thread), so poll for it.
  write_input("input.txt", "v2-changed");

  std::string content;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    std::this_thread::sleep_for(50ms);
    content = read_output(tree, "output.txt");
  } while (content != "v2-changed" &&
           std::chrono::steady_clock::now() < deadline);

  EXPECT_EQ(content, "v2-changed");
}
#endif

}  // namespace
