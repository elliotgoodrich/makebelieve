// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
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
  std::string_view wildcard_source = {};
  bool wildcard_over_outputs = false;
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
    EXPECT_EQ(manifest.rules()[i].wildcard_source,
              c.expected[i].wildcard_source);
    EXPECT_EQ(manifest.rules()[i].wildcard_over_outputs,
              c.expected[i].wildcard_over_outputs);
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
        // A copy source under `@/` is another output, which may be declared
        // before or after the copy; it is kept normalised with its `@/`, and
        // `@/` in a command is left for the runner.
        ParseCase{.name = "copies_another_output",
                  .input = "@/a.txt = copy @/./out/../b.txt\n"
                           "@/b.txt = capture cat @/c.txt\n"
                           "@/c.txt = run build %out\n"
                           "@/d.txt = copy @/a.txt\n",
                  .expected = {{"a.txt", Action::Copy, "@/b.txt"},
                               {"b.txt", Action::Capture, "cat @/c.txt"},
                               {"c.txt", Action::Run, "build %out"},
                               {"d.txt", Action::Copy, "@/a.txt"}}},
        // It must name a file inside `@/` that the manifest declares, and not
        // the copy's own output; each error lands in line order with the rest.
        ParseCase{.name = "rejects_a_copy_of_an_undeclared_output",
                  .input = "@/a.txt = copy @/missing.txt\n"
                           "@/b.txt = copy @/../escape.txt\n"
                           "@/c.txt = copy @/c.txt\n"
                           "@/d.txt = copy @/dir\n"
                           "not a rule\n"
                           "@/dir/e.txt = run e\n"
                           "@/f.txt = copy @/dir/e.txt\n",
                  .expected = {{"dir/e.txt", Action::Run, "e"},
                               {"f.txt", Action::Copy, "@/dir/e.txt"}},
                  .error_lines = {1, 2, 3, 4, 5}},
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
        // A `*` in the output makes a wildcard rule, whose sources are those
        // of the first word of the command to hold a `*`, normalised; the
        // command itself is kept verbatim, later `*`s and all.
        ParseCase{.name = "wildcard_run_rule",
                  .input = "@/out/*.o = run cc -c ./src/*.cpp -o %out # *\n",
                  .expected = {{"out/*.o", Action::Run,
                                "cc -c ./src/*.cpp -o %out # *", "src/*.cpp"}}},
        // A wildcard copy's whole path is its pattern.
        ParseCase{.name = "wildcard_copy_rule",
                  .input = "@/*.txt = copy my notes/note-*.md\n",
                  .expected = {{"*.txt", Action::Copy, "my notes/note-*.md",
                                "my notes/note-*.md"}}},
        // Without a `*` in the output, one in the command is the shell's.
        ParseCase{.name = "a_star_in_a_simple_rules_command_is_not_a_wildcard",
                  .input = "@/all.txt = capture cat src/*.txt\n",
                  .expected = {{"all.txt", Action::Capture, "cat src/*.txt"}}},
        // Both patterns need exactly one `*`, in the file name, and the
        // sources must be source files the build can watch.
        ParseCase{.name = "rejects_a_malformed_wildcard",
                  .input = "@/*/a.o = run cc src/*.cpp\n"
                           "@/*-*.o = run cc src/*.cpp\n"
                           "@/*.o = run cc src/main.cpp\n"
                           "@/*.o = run cc */main.cpp\n"
                           "@/*.o = run cc src/*-*.cpp\n"
                           "@/*.o = run cc ../*.cpp\n"
                           "@/*.o = run cc \"src/*.cpp\"\n"
                           "@/*.o = run cc @/../gen/*.cpp\n"
                           "@/*.o = copy @/gen/*/a.o\n"
                           "@/*.json = tracing\n",
                  .expected = {},
                  .error_lines = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
        // A wildcard bans what its outputs end in - whatever follows the `*` -
        // from its directory: no other output there may end in it, whichever
        // is declared first and whatever comes before.
        ParseCase{
            .name = "rejects_an_output_ending_in_a_banned_suffix",
            .input = "@/out/*.foo = copy src/*.txt\n"
                     "@/out/a.foo = run a\n"
                     "@/b.bar = run b\n"
                     "@/lib*.bar = copy src/*.txt\n"
                     "@/out/.foo = run c\n"
                     "@/a.foo = run d\n"
                     "@/other/a.foo = run e\n"
                     "@/out/a.food = run f\n",
            .expected = {{"out/*.foo", Action::Copy, "src/*.txt", "src/*.txt"},
                         {"b.bar", Action::Run, "b"},
                         {"a.foo", Action::Run, "d"},
                         {"other/a.foo", Action::Run, "e"},
                         {"out/a.food", Action::Run, "f"}},
            .error_lines = {2, 4, 5}},
        ParseCase{
            .name = "rejects_a_wildcard_banning_a_declared_output",
            .input = "@/out/a.foo = run a\n"
                     "@/out/*.foo = copy src/*.txt\n"
                     "@/out/a* = copy src/*.txt\n"
                     "@/out/*.bar = copy src/*.txt\n",
            .expected = {{"out/a.foo", Action::Run, "a"},
                         {"out/*.bar", Action::Copy, "src/*.txt", "src/*.txt"}},
            .error_lines = {2, 3}},
        // Nor may a wildcard produce a file where another output needs a
        // directory, in either order.
        ParseCase{.name = "rejects_a_wildcard_that_could_produce_a_directory",
                  .input = "@/gen/x.d/file = run a\n"
                           "@/gen/*.d = copy src/*.c\n"
                           "@/*.o = copy src/*.c\n"
                           "@/main.o/file = run b\n"
                           "@/main.o/*.x = copy src/*.c\n",
                  .expected = {{"gen/x.d/file", Action::Run, "a"},
                               {"*.o", Action::Copy, "src/*.c", "src/*.c"}},
                  .error_lines = {2, 4, 5}},
        // Two wildcards in one directory clash when either's suffix ends in
        // the other's, whatever comes before their `*`s.
        ParseCase{.name = "rejects_wildcards_sharing_a_suffix",
                  .input = "@/*.o = copy a/*.c\n"
                           "@/lib*.o = copy b/*.c\n"
                           "@/a* = copy c/*.c\n"
                           "@/*.d = copy a/*.c\n"
                           "@/lib*.a = copy b/*.c\n"
                           "@/sub/*.o = copy b/*.c\n"
                           "@/*.x.o = copy d/*.c\n"
                           "@/*d = copy d/*.c\n"
                           "@/bin*.a = copy d/*.c\n",
                  .expected = {{"*.o", Action::Copy, "a/*.c", "a/*.c"},
                               {"*.d", Action::Copy, "a/*.c", "a/*.c"},
                               {"lib*.a", Action::Copy, "b/*.c", "b/*.c"},
                               {"sub/*.o", Action::Copy, "b/*.c", "b/*.c"}},
                  .error_lines = {2, 3, 7, 8, 9}},
        // A source pattern under `@/` matches outputs instead of source
        // files, whatever declares them, and is kept without its `@/`.
        ParseCase{
            .name = "wildcard_over_outputs",
            .input = "@/gen/*.md = capture render src/*.txt\n"
                     "@/html/*.html = run pandoc @/./gen/*.md -o %out\n"
                     "@/site/*.md = copy @/gen/*.md\n",
            .expected =
                {{"gen/*.md", Action::Capture, "render src/*.txt", "src/*.txt"},
                 {"html/*.html", Action::Run, "pandoc @/./gen/*.md -o %out",
                  "gen/*.md", true},
                 {"site/*.md", Action::Copy, "@/gen/*.md", "gen/*.md", true}}},
        // A wildcard that could match what it produces - itself, or through
        // others - would declare outputs without end.
        ParseCase{
            .name = "rejects_wildcards_that_feed_themselves",
            .input = "@/gen/*.md.md = copy @/gen/*.md\n"
                     "@/a/*.x = copy @/b/*.y\n"
                     "@/b/lib*.y = copy @/c/*.z\n"
                     "@/c/*.z = copy @/a/*.x\n"
                     "@/d/*.x = copy @/a/*.x\n"
                     "@/e/*.md = copy @/e/sub/*.md\n"
                     "@/f/*.html = copy @/f/*.md\n",
            .expected =
                {{"d/*.x", Action::Copy, "@/a/*.x", "a/*.x", true},
                 {"e/*.md", Action::Copy, "@/e/sub/*.md", "e/sub/*.md", true},
                 {"f/*.html", Action::Copy, "@/f/*.md", "f/*.md", true}},
            .error_lines = {1, 2, 3, 4}},
        ParseCase{
            .name = "handles_a_final_line_without_a_newline",
            .input = "@/output.txt = run cp input.txt %out",
            .expected = {{"output.txt", Action::Run, "cp input.txt %out"}}}),
    [](const ::testing::TestParamInfo<ParseCase>& info) {
      return std::string(info.param.name);
    });

// A wildcard rule stands for one simple rule per matching source name, with
// whatever the `*` matched put into the output and every `*` of the command.
TEST(ManifestInstantiate, SubstitutesTheStem) {
  const makebelieve::Manifest manifest = makebelieve::Manifest::parse(
      "@/out/lib*.o = run cc -c src/*.cpp -MF *.d -o %out\n");
  ASSERT_EQ(manifest.rules().size(), 1U);
  const makebelieve::Manifest::Rule& rule = manifest.rules()[0];
  ASSERT_TRUE(rule.is_wildcard());

  const auto matched = makebelieve::Manifest::instantiate(rule, "main.cpp");
  ASSERT_TRUE(matched.has_value());
  EXPECT_EQ(matched->output, std::filesystem::path("out") / "libmain.o");
  EXPECT_EQ(matched->action, Action::Run);
  EXPECT_EQ(matched->command, "cc -c src/main.cpp -MF main.d -o %out");
  EXPECT_FALSE(matched->is_wildcard());

  // The `*` matches at least one character, and the whole name must match.
  EXPECT_FALSE(makebelieve::Manifest::instantiate(rule, ".cpp").has_value());
  EXPECT_FALSE(makebelieve::Manifest::instantiate(rule, "main.c").has_value());
  EXPECT_FALSE(
      makebelieve::Manifest::instantiate(rule, "main.cpp.bak").has_value());
}

}  // namespace
