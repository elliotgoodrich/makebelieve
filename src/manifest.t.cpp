// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The rule a case expects, spelled with string_views so cases stay literals.
struct ExpectedRule {
  std::string_view output;
  std::string_view command;
};

// One parsing case: a manifest, the rules it should yield, and an identifier
// that names the case in the test output.
struct ParseCase {
  std::string_view name;
  std::string_view input;
  std::vector<ExpectedRule> expected;
};

class ManifestParse : public ::testing::TestWithParam<ParseCase> {};

TEST_P(ManifestParse, YieldsExpectedRules) {
  const ParseCase& c = GetParam();
  const makebelieve::Manifest manifest = makebelieve::Manifest::parse(c.input);

  ASSERT_EQ(manifest.rules().size(), c.expected.size());
  for (std::size_t i = 0; i < c.expected.size(); ++i) {
    EXPECT_EQ(manifest.rules()[i].output, c.expected[i].output);
    EXPECT_EQ(manifest.rules()[i].command, c.expected[i].command);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Manifest,
    ManifestParse,
    ::testing::Values(
        ParseCase{.name = "empty_text", .input = "", .expected = {}},
        ParseCase{.name = "single_rule",
                  .input = "@/output.txt <- cp input.txt %out\n",
                  .expected = {{"output.txt", "cp input.txt %out"}}},
        // The `@/` prefix is stripped and surrounding whitespace trimmed from
        // both the output and the command.
        ParseCase{.name = "strips_prefix_and_trims_whitespace",
                  .input = "   @/out/foo.o   <-   build foo   \n",
                  .expected = {{"out/foo.o", "build foo"}}},
        ParseCase{.name = "keeps_declaration_order",
                  .input = "@/one.txt <- a\n"
                           "@/two.txt <- b\n"
                           "@/three.txt <- c\n",
                  .expected = {{"one.txt", "a"},
                               {"two.txt", "b"},
                               {"three.txt", "c"}}},
        ParseCase{.name = "skips_comments_and_blank_lines",
                  .input = "# a comment\n"
                           "\n"
                           "   \n"
                           "@/output.txt <- cp input.txt %out\n"
                           "# trailing comment\n",
                  .expected = {{"output.txt", "cp input.txt %out"}}},
        // A path without the `@/` prefix is outside the output namespace, and a
        // line with no `<-` is not a rule at all; both are skipped.
        ParseCase{.name = "skips_lines_outside_the_output_namespace",
                  .input = "plain.txt <- cp input.txt %out\n"
                           "just some words\n"
                           "@/kept.txt <- cp input.txt %out\n",
                  .expected = {{"kept.txt", "cp input.txt %out"}}},
        ParseCase{.name = "skips_empty_output_or_command",
                  .input = "@/ <- cp input.txt %out\n"
                           "@/output.txt <-\n",
                  .expected = {}},
        ParseCase{.name = "handles_a_final_line_without_a_newline",
                  .input = "@/output.txt <- cp input.txt %out",
                  .expected = {{"output.txt", "cp input.txt %out"}}}),
    [](const ::testing::TestParamInfo<ParseCase>& info) {
      return std::string(info.param.name);
    });

}  // namespace
