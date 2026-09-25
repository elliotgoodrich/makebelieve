// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Action = makebelieve::Manifest::Action;

// The rule a case expects, spelled with string_views so cases stay literals.
struct ExpectedRule {
  std::string_view output;
  Action action;
  std::string_view command;
};

// One parsing case: a manifest, the rules it should yield, the lines it should
// reject, and an identifier that names the case in the test output.
struct ParseCase {
  std::string_view name;
  std::string_view input;
  std::vector<ExpectedRule> expected;
  std::vector<std::size_t> error_lines = {};
};

class ManifestParse : public ::testing::TestWithParam<ParseCase> {};

TEST_P(ManifestParse, YieldsExpectedRules) {
  const ParseCase& c = GetParam();
  const makebelieve::Manifest manifest = makebelieve::Manifest::parse(c.input);

  ASSERT_EQ(manifest.rules().size(), c.expected.size());
  for (std::size_t i = 0; i < c.expected.size(); ++i) {
    EXPECT_EQ(manifest.rules()[i].output, c.expected[i].output);
    EXPECT_EQ(manifest.rules()[i].action, c.expected[i].action);
    EXPECT_EQ(manifest.rules()[i].command, c.expected[i].command);
  }

  std::vector<std::size_t> error_lines;
  for (const makebelieve::Manifest::Error& error : manifest.errors()) {
    error_lines.push_back(error.line);
    EXPECT_FALSE(error.message.empty());
  }
  EXPECT_EQ(error_lines, c.error_lines);
}

INSTANTIATE_TEST_SUITE_P(
    Manifest,
    ManifestParse,
    ::testing::Values(
        ParseCase{.name = "empty_text", .input = "", .expected = {}},
        ParseCase{
            .name = "single_run_rule",
            .input = "@/output.txt = run cp input.txt %out\n",
            .expected = {{"output.txt", Action::Run, "cp input.txt %out"}}},
        // `capture` takes the same kind of command, but the bytes it writes to
        // standard output are the ones that count.
        ParseCase{
            .name = "single_capture_rule",
            .input = "@/output.txt = capture echo \"foo\"\n",
            .expected = {{"output.txt", Action::Capture, "echo \"foo\""}}},
        // `copy` names a file instead of a command; the rule keeps it, made
        // relative and normalised, in the same field.
        ParseCase{.name = "single_copy_rule",
                  .input = "@/output.txt = copy src/input.txt\n",
                  .expected = {{"output.txt", Action::Copy, "src/input.txt"}}},
        ParseCase{.name = "normalises_a_copy_source",
                  .input = "@/output.txt = copy ./src/../input.txt\n",
                  .expected = {{"output.txt", Action::Copy, "input.txt"}}},
        // A copy source has to name a file inside the manifest's directory,
        // so every copy has a source the build can watch.
        ParseCase{.name = "rejects_a_copy_source_outside_the_directory",
                  .input = "@/a.txt = copy ../outside.txt\n"
                           "@/b.txt = copy /etc/hosts\n"
                           "@/c.txt = copy subdir/\n"
                           "@/d.txt = copy .\n"
                           "@/e.txt = copy kept.txt\n",
                  .expected = {{"e.txt", Action::Copy, "kept.txt"}},
                  .error_lines = {1, 2, 3, 4}},
        // `tracing` takes nothing after it: the output is makebelieve's own
        // trace.
        ParseCase{.name = "single_tracing_rule",
                  .input = "@/trace.json = tracing  \n",
                  .expected = {{"trace.json", Action::Tracing, ""}}},
        ParseCase{.name = "rejects_a_tracing_rule_with_an_argument",
                  .input = "@/a.json = tracing everything\n"
                           "@/b.json = tracing\n",
                  .expected = {{"b.json", Action::Tracing, ""}},
                  .error_lines = {1}},
        // A copy source is a path, not a command line, so the whole of the
        // rest of the line is the path - spaces and all.
        ParseCase{.name = "keeps_spaces_in_a_copy_source",
                  .input = "@/output.txt = copy my input.txt\n",
                  .expected = {{"output.txt", Action::Copy, "my input.txt"}}},
        // The `@/` prefix is stripped and surrounding whitespace trimmed from
        // the output, the action and the command.
        ParseCase{.name = "strips_prefix_and_trims_whitespace",
                  .input = "   @/out/foo.o   =   run   build foo   \n",
                  .expected = {{"out/foo.o", Action::Run, "build foo"}}},
        ParseCase{.name = "keeps_declaration_order",
                  .input = "@/one.txt = run a\n"
                           "@/two.txt = capture b\n"
                           "@/three.txt = copy c\n"
                           "@/four.txt = run d\n",
                  .expected = {{"one.txt", Action::Run, "a"},
                               {"two.txt", Action::Capture, "b"},
                               {"three.txt", Action::Copy, "c"},
                               {"four.txt", Action::Run, "d"}}},
        ParseCase{
            .name = "skips_comments_and_blank_lines",
            .input = "# a comment\n"
                     "\n"
                     "   \n"
                     "@/output.txt = run cp input.txt %out\n"
                     "# trailing comment\n",
            .expected = {{"output.txt", Action::Run, "cp input.txt %out"}}},
        // A path without the `@/` prefix is outside the output namespace, and
        // a line with no `=` is not a rule at all; both are rejected.
        ParseCase{.name = "rejects_lines_outside_the_output_namespace",
                  .input = "plain.txt = run cp input.txt %out\n"
                           "just some words\n"
                           "@/kept.txt = run cp input.txt %out\n",
                  .expected = {{"kept.txt", Action::Run, "cp input.txt %out"}},
                  .error_lines = {1, 2}},
        ParseCase{.name = "rejects_empty_output_or_command",
                  .input = "@/ = run cp input.txt %out\n"
                           "@/output.txt = run\n"
                           "@/output.txt = capture\n"
                           "@/output.txt = copy\n"
                           "@/output.txt =\n",
                  .expected = {},
                  .error_lines = {1, 2, 3, 4, 5}},
        // Only `run`, `capture`, `copy` and `tracing` name an action; anything
        // else - a bare command, a word that merely starts with one of them, or
        // a rule invocation - is rejected rather than guessed at.
        ParseCase{.name = "rejects_an_unknown_action",
                  .input = "@/output.txt = cp input.txt %out\n"
                           "@/output.txt = running cp input.txt %out\n"
                           "@/output.txt = copying input.txt\n"
                           "@/output.txt = tracingfile\n"
                           "@/foo.o = !cc foo.cpp\n",
                  .expected = {},
                  .error_lines = {1, 2, 3, 4, 5}},
        // Syntax the manifest format documents but the parser does not handle
        // yet is rejected rather than misread.
        ParseCase{.name = "rejects_unsupported_syntax",
                  .input = "flags = -O2\n"
                           "rule cc = clang++ -c %in -o %out\n",
                  .expected = {},
                  .error_lines = {1, 2}},
        ParseCase{.name = "rejects_outputs_outside_the_output_directory",
                  .input = "@/../escape.txt = run a\n"
                           "@/dir/ = run b\n"
                           "@/. = run c\n"
                           "@/a/.. = run d\n",
                  .expected = {},
                  .error_lines = {1, 2, 3, 4}},
        ParseCase{.name = "normalises_outputs",
                  .input = "@/a/./b/../c.txt = run a\n",
                  .expected = {{"a/c.txt", Action::Run, "a"}}},
        ParseCase{.name = "rejects_an_output_declared_twice",
                  .input = "@/output.txt = run a\n"
                           "@/output.txt = run b\n",
                  .expected = {{"output.txt", Action::Run, "a"}},
                  .error_lines = {2}},
        // One path cannot be both a file and a directory, in either order.
        ParseCase{.name = "rejects_an_output_inside_another",
                  .input = "@/out = run a\n"
                           "@/out/file.txt = run b\n",
                  .expected = {{"out", Action::Run, "a"}},
                  .error_lines = {2}},
        ParseCase{.name = "rejects_an_output_that_is_a_directory_of_another",
                  .input = "@/out/deep/file.txt = run a\n"
                           "@/out/deep = run b\n"
                           "@/out/other.txt = run c\n",
                  .expected = {{"out/deep/file.txt", Action::Run, "a"},
                               {"out/other.txt", Action::Run, "c"}},
                  .error_lines = {2}},
        ParseCase{
            .name = "handles_a_final_line_without_a_newline",
            .input = "@/output.txt = run cp input.txt %out",
            .expected = {{"output.txt", Action::Run, "cp input.txt %out"}}}),
    [](const ::testing::TestParamInfo<ParseCase>& info) {
      return std::string(info.param.name);
    });

}  // namespace
