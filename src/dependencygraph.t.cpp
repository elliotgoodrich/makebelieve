// SPDX-License-Identifier: MIT
#include "dependencygraph.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace makebelieve;

namespace {

using Kind = BuildCache::Input::Kind;
using Options = DependencyGraph::Options;
using Prefix = DependencyGraph::Prefix;

BuildCache::Input input(const char* path, Kind kind) {
  return BuildCache::Input{.path = path, .kind = kind};
}

BuildCache::Record record(const char* output,
                          std::vector<BuildCache::Input> inputs) {
  return BuildCache::Record{.command = Command{.output = output,
                                               .action = Manifest::Action::Run,
                                               .text = "c"},
                            .hash = {},
                            .mtime = {},
                            .inputs = std::move(inputs)};
}

// top reads left and right, which both read shared, which reads leaf.txt.
DependencyGraph diamond() {
  const std::vector<BuildCache::Record> records{
      record("top",
             {input("@/right", Kind::output), input("@/left", Kind::output)}),
      record("left", {input("@/shared", Kind::output)}),
      record("right", {input("@/shared", Kind::output)}),
      record("shared", {input("leaf.txt", Kind::file)}),
  };
  return DependencyGraph(records);
}

// What @a graph prints in ASCII, which keeps the trees below readable whatever
// the compiler takes this file's encoding to be.
std::string print(const DependencyGraph& graph,
                  std::vector<std::string> paths,
                  Options options = {}) {
  options.charset = DependencyGraph::Charset::ascii;
  auto printed = graph.print(paths, options);
  EXPECT_TRUE(printed.has_value()) << printed.error();
  return printed.value_or("");
}

}  // namespace

TEST(DependencyGraph, DrawsWithBoxCharactersByDefault) {
  const std::vector<std::string> paths{"@/top"};
  // U+251C, U+2514, U+2502 and U+2500.
  const std::string branch = "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ";
  const std::string last = "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ";
  const std::string pipe = "\xE2\x94\x82   ";
  EXPECT_EQ(diamond().print(paths, {}).value(),
            "@/top\n" + branch + "@/left\n" + pipe + last + "@/shared\n" +
                pipe + "    " + last + "leaf.txt\n" + last + "@/right\n" +
                "    " + last + "@/shared (*)\n");
}

TEST(DependencyGraph, PrintsATreeMarkingRepeats) {
  EXPECT_EQ(print(diamond(), {"@/top"}),
            "@/top\n"
            "|-- @/left\n"
            "|   `-- @/shared\n"
            "|       `-- leaf.txt\n"
            "`-- @/right\n"
            "    `-- @/shared (*)\n");
}

TEST(DependencyGraph, NoDedupeRepeatsWhatIsBeneath) {
  EXPECT_EQ(print(diamond(), {"@/top"}, {.no_dedupe = true}),
            "@/top\n"
            "|-- @/left\n"
            "|   `-- @/shared\n"
            "|       `-- leaf.txt\n"
            "`-- @/right\n"
            "    `-- @/shared\n"
            "        `-- leaf.txt\n");
}

TEST(DependencyGraph, InvertPrintsDependents) {
  EXPECT_EQ(print(diamond(), {"leaf.txt"}, {.invert = true}),
            "leaf.txt\n"
            "`-- @/shared\n"
            "    |-- @/left\n"
            "    |   `-- @/top\n"
            "    `-- @/right\n"
            "        `-- @/top\n");
}

TEST(DependencyGraph, DepthLimitsHowFarItGoes) {
  EXPECT_EQ(print(diamond(), {"@/top"}, {.depth = 1}),
            "@/top\n"
            "|-- @/left\n"
            "`-- @/right\n");
  EXPECT_EQ(print(diamond(), {"@/top"}, {.depth = 0}), "@/top\n");
}

TEST(DependencyGraph, ADepthLimitedRepeatStillShowsWhatIsWithinIt) {
  const std::vector<BuildCache::Record> records{
      record("top", {input("@/a", Kind::output), input("@/b", Kind::output)}),
      record("a", {input("@/b", Kind::output)}),
      record("b", {input("@/c", Kind::output)}),
      record("c", {input("leaf.txt", Kind::file)}),
  };
  EXPECT_EQ(print(DependencyGraph(records), {"@/top"}, {.depth = 2}),
            "@/top\n"
            "|-- @/a\n"
            "|   `-- @/b\n"
            "`-- @/b\n"
            "    `-- @/c\n");
}

TEST(DependencyGraph, PrefixDepthNumbersEachLine) {
  EXPECT_EQ(print(diamond(), {"@/left"}, {.prefix = Prefix::depth}),
            "0@/left\n"
            "1@/shared\n"
            "2leaf.txt\n");
}

TEST(DependencyGraph, PrefixNoneListsEachPathOnce) {
  EXPECT_EQ(print(diamond(), {"@/top"}, {.prefix = Prefix::none}),
            "@/left\n@/right\n@/shared\nleaf.txt\n");
  EXPECT_EQ(
      print(diamond(), {"leaf.txt"}, {.invert = true, .prefix = Prefix::none}),
      "@/left\n@/right\n@/shared\n@/top\n");
  EXPECT_EQ(print(diamond(), {"@/top"}, {.depth = 1, .prefix = Prefix::none}),
            "@/left\n@/right\n");
}

TEST(DependencyGraph, HidesLookedAtPathsUnlessAskedForAll) {
  const std::vector<BuildCache::Record> records{record(
      "out", {input("dir", Kind::directory), input("listed", Kind::listing),
              input("nope", Kind::missing), input("file", Kind::file)})};
  const DependencyGraph graph(records);
  EXPECT_EQ(print(graph, {"@/out"}),
            "@/out\n"
            "|-- file\n"
            "`-- listed (listed)\n");
  EXPECT_EQ(print(graph, {"@/out"}, {.all = true}),
            "@/out\n"
            "|-- dir (directory)\n"
            "|-- file\n"
            "|-- listed (listed)\n"
            "`-- nope (missing)\n");
  EXPECT_EQ(print(graph, {"nope"}, {.invert = true}),
            "nope\n"
            "`-- @/out\n");
}

TEST(DependencyGraph, TheSourceRootIsADot) {
  const std::vector<BuildCache::Record> records{
      record("out", {input("", Kind::listing), input("@", Kind::listing)})};
  const DependencyGraph graph(records);
  EXPECT_EQ(print(graph, {"@/out"}),
            "@/out\n"
            "|-- . (listed)\n"
            "`-- @ (listed)\n");
  for (const char* const root : {".", "./"}) {
    EXPECT_EQ(print(graph, {root}, {.invert = true}),
              ".\n"
              "`-- @/out\n");
  }
  EXPECT_EQ(print(graph, {"@/"}, {.invert = true}),
            "@\n"
            "`-- @/out\n");
}

TEST(DependencyGraph, ACycleEnds) {
  const std::vector<BuildCache::Record> records{
      record("a", {input("@/b", Kind::output)}),
      record("b", {input("@/a", Kind::output)}),
  };
  EXPECT_EQ(print(DependencyGraph(records), {"@/a"}, {.no_dedupe = true}),
            "@/a\n"
            "`-- @/b\n"
            "    `-- @/a (*)\n");
}

TEST(DependencyGraph, SeveralPathsAreSeparatedInATree) {
  EXPECT_EQ(print(diamond(), {"@/shared", "@/left"}, {.depth = 1}),
            "@/shared\n"
            "`-- leaf.txt\n"
            "\n"
            "@/left\n"
            "`-- @/shared\n");
}

TEST(DependencyGraph, FailsForWhatIsNotRecorded) {
  const DependencyGraph graph = diamond();
  const std::vector<std::string> unbuilt{"@/unbuilt"};
  EXPECT_FALSE(graph.print(unbuilt, {}).has_value());
  EXPECT_FALSE(graph.print(unbuilt, {.invert = true}).has_value());
  // A source has dependents, but no recorded dependencies.
  const std::vector<std::string> source{"leaf.txt"};
  EXPECT_FALSE(graph.print(source, {}).has_value());
  EXPECT_TRUE(graph.print(source, {.invert = true}).has_value());
}
