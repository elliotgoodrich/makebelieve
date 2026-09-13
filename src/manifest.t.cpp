// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
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

TEST(ManifestPlaceholders, ResolvesAllThreeLevelsIncludingForwardDeclarations) {
  const auto manifest = makebelieve::Manifest::parse(
      "@/index.html <- page: index.md\n"
      "    %placeholder = special.html\n"
      "@/guide.html <- page: guide.md\n"
      "@/raw.html <- echo raw\n"
      "rule page = pandoc %in --to=html5 -o %out\n"
      "    %placeholder = rule.html\n"
      "[*.html]\n"
      "    %placeholder = default.html\n");
  ASSERT_EQ(manifest.rules().size(), 3U);
  EXPECT_EQ(manifest.rules()[0].placeholder, "special.html");
  EXPECT_EQ(manifest.rules()[1].placeholder, "rule.html");
  EXPECT_EQ(manifest.rules()[2].placeholder, "default.html");
  EXPECT_EQ(manifest.rules()[0].command, "pandoc index.md --to=html5 -o %out");
  EXPECT_EQ(manifest.rules()[1].command, "pandoc guide.md --to=html5 -o %out");
}

TEST(ManifestPlaceholders, MatchesBasenamesAndDistinguishesTrailingDot) {
  const auto manifest = makebelieve::Manifest::parse(
      "[*.html]\n  %placeholder = page.html\n"
      "[no extension]\n  %placeholder = noext\n"
      "[*.]\n  %placeholder = dot\n"
      "[*]\n  %placeholder = fallback\n"
      "@/nested/index.html <- build\n"
      "@/LICENSE <- build\n"
      "@/.hidden <- build\n"
      "@/trailing. <- build\n"
      "@/style.css <- build\n");
  ASSERT_EQ(manifest.rules().size(), 5U);
  EXPECT_EQ(manifest.rules()[0].placeholder, "page.html");
  EXPECT_EQ(manifest.rules()[1].placeholder, "noext");
  EXPECT_EQ(manifest.rules()[2].placeholder, "noext");
  EXPECT_EQ(manifest.rules()[3].placeholder, "dot");
  EXPECT_EQ(manifest.rules()[4].placeholder, "fallback");
}

TEST(ManifestPlaceholders, ExactMatchWinsAndLastEqualSpecificityWins) {
  const auto manifest = makebelieve::Manifest::parse(
      "[index.html]\n  %placeholder = exact\n"
      "[*.html]\n  %placeholder = first\n"
      "[*.html]\n  %placeholder = second\n"
      "@/index.html <- build\n"
      "@/other.html <- build\n");
  ASSERT_EQ(manifest.rules().size(), 2U);
  EXPECT_EQ(manifest.rules()[0].placeholder, "exact");
  EXPECT_EQ(manifest.rules()[1].placeholder, "second");
}

TEST(ManifestPlaceholders, SettingsAreIndentedAndDoNotLeakBetweenOutputs) {
  const auto manifest = makebelieve::Manifest::parse(
      "@/first <- build\r\n"
      "\t# preserve block through comments\r\n\r\n"
      "\tflags = -O2\r\n"
      "\t%placeholder = \"placeholders/loading page.html\"\r\n"
      "@/second <- C:\\tools\\build.exe\r\n"
      "\t%placeholder = second.html\r\n"
      "@/third <- build\r\n");
  ASSERT_EQ(manifest.rules().size(), 3U);
  EXPECT_EQ(manifest.rules()[0].placeholder, "placeholders/loading page.html");
  EXPECT_EQ(manifest.rules()[1].placeholder, "second.html");
  EXPECT_FALSE(manifest.rules()[2].placeholder.has_value());
  EXPECT_EQ(manifest.rules()[1].command, "C:\\tools\\build.exe");
}

// An indented declaration closes the block above it rather than being read as
// one of its settings, even though it looks like one.
TEST(ManifestPlaceholders, AnIndentedDeclarationClosesTheBlockAboveIt) {
  const auto manifest = makebelieve::Manifest::parse(
      "@/first <- build\n"
      "    %placeholder = first.html\n"
      "    @/second <- build --mode=fast\n"
      "        %placeholder = second.html\n");
  ASSERT_EQ(manifest.rules().size(), 2U);
  EXPECT_EQ(manifest.rules()[0].placeholder, "first.html");
  EXPECT_EQ(manifest.rules()[1].placeholder, "second.html");
  EXPECT_EQ(manifest.rules()[1].command, "build --mode=fast");
}

// A `%` token is only replaced when it stands alone, so a short name cannot
// corrupt a longer one that happens to start with it.
TEST(ManifestPlaceholders, SubstitutionStopsAtTokenBoundaries) {
  const auto manifest = makebelieve::Manifest::parse(
      "rule r = tool --input=%input %in --in %ins -o %out\n"
      "@/a <- r: page.md\n");
  ASSERT_EQ(manifest.rules().size(), 1U);
  EXPECT_EQ(manifest.rules()[0].command,
            "tool --input=%input page.md --in %ins -o %out");
  EXPECT_EQ(makebelieve::Manifest::substitute("%out %output %out", "%out", "x"),
            "x %output x");
}

// `%out` is quoted for the shell, so `%in` is too - but only when it has
// whitespace to protect, since a wildcard must stay unquoted.
TEST(ManifestPlaceholders, QuotesAnInputOnlyWhenItNeedsIt) {
  const auto manifest = makebelieve::Manifest::parse(
      "rule r = tool %in -o %out\n"
      "@/a <- r: my page.md\n"
      "@/b <- r: page.md\n"
      "@/c <- r: \"already quoted.md\"\n"
      "@/d <- r: src/*.md\n");
  ASSERT_EQ(manifest.rules().size(), 4U);
  EXPECT_EQ(manifest.rules()[0].command, "tool \"my page.md\" -o %out");
  EXPECT_EQ(manifest.rules()[1].command, "tool page.md -o %out");
  EXPECT_EQ(manifest.rules()[2].command, "tool \"already quoted.md\" -o %out");
  EXPECT_EQ(manifest.rules()[3].command, "tool src/*.md -o %out");
}

// Only `identifier:` followed by whitespace invokes a rule, so an ordinary
// command containing a colon is never silently rewritten - not even when a
// declared rule shares the name, which a drive letter otherwise would.
TEST(ManifestPlaceholders, OnlyAnIdentifierBeforeTheColonInvokesARule) {
  const auto manifest = makebelieve::Manifest::parse(
      "rule C = tool %in\n"
      "rule build-page = page %in\n"
      "@/a <- C:\\tools\\build.exe\n"
      "@/b <- curl https://example.com -o %out\n"
      "@/c <- powershell -c \"echo hi\" : 2\n"
      "@/d <- build-page: index.md\n");
  ASSERT_EQ(manifest.rules().size(), 4U);
  EXPECT_EQ(manifest.rules()[0].command, "C:\\tools\\build.exe");
  EXPECT_EQ(manifest.rules()[1].command, "curl https://example.com -o %out");
  EXPECT_EQ(manifest.rules()[2].command, "powershell -c \"echo hi\" : 2");
  EXPECT_EQ(manifest.rules()[3].command, "page index.md");
}

// Syntax the parser understands but cannot honour is reported rather than
// quietly dropped.
TEST(ManifestPlaceholders, RejectsSyntaxItCannotHonour) {
  const auto rejects = [](std::string_view text) {
    EXPECT_THROW((void)makebelieve::Manifest::parse(text),
                 std::invalid_argument)
        << text;
  };
  rejects("[*]\n  %placeholder = \"\"\n");
  rejects("[*]\n  %placeholder =\n");
  rejects("%placeholder = outside-any-block\n");
  rejects("[]\n  %placeholder = p\n");
  rejects("[*.min.*]\n  %placeholder = p\n");
  rejects("rule broken\n");
  rejects("rule broken =\n");
  rejects("@/a <- build\n  a setting with no equals sign\n");
  rejects("@/a.o <- cc: foo.cpp\n");  // No `rule cc` is declared.
}

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
