// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Action = makebelieve::Manifest::Action;

// The rule a case expects, spelled with string_views so cases stay literals.
struct ExpectedRule {
  std::string_view output;
  Action action;
  std::string_view command;
  // For a pattern rule, its source pattern without any `@/`.
  std::string_view source = {};
  bool over_outputs = false;
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
    const makebelieve::Manifest::Pattern* pattern =
        manifest.rules()[i].pattern.get();
    EXPECT_EQ(pattern != nullptr ? pattern->source : std::string(),
              c.expected[i].source);
    EXPECT_EQ(pattern != nullptr && pattern->over_outputs,
              c.expected[i].over_outputs);
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
        // Nor does `status`: the output is what is building and failing.
        ParseCase{.name = "single_status_rule",
                  .input = "@/status.txt = status\n",
                  .expected = {{"status.txt", Action::Status, ""}}},
        ParseCase{.name = "rejects_a_status_rule_with_an_argument",
                  .input = "@/a.txt = status everything\n"
                           "@/b.txt = status\n",
                  .expected = {{"b.txt", Action::Status, ""}},
                  .error_lines = {1}},
        // A copy source is a path, not a command line, so the whole of the
        // rest of the line is the path - spaces and all.
        ParseCase{.name = "keeps_spaces_in_a_copy_source",
                  .input = "@/output.txt = copy my input.txt\n",
                  .expected = {{"output.txt", Action::Copy, "my input.txt"}}},
        // The `@/` prefix is stripped and surrounding whitespace trimmed from
        // the output, the action and the command - but not from the start of
        // the line, where it would make the line a variable.
        ParseCase{.name = "strips_prefix_and_trims_whitespace",
                  .input = "@/out/foo.o   =   run   build foo   \n",
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
        // Only `run`, `capture`, `copy`, `tracing` and `status` name an
        // action; anything else - a bare command, a word that merely starts
        // with one of them, or a rule invocation - is rejected rather than
        // guessed at.
        ParseCase{.name = "rejects_an_unknown_action",
                  .input = "@/output.txt = cp input.txt %out\n"
                           "@/output.txt = running cp input.txt %out\n"
                           "@/output.txt = copying input.txt\n"
                           "@/output.txt = tracingfile\n"
                           "@/output.txt = statusfile\n"
                           "@/foo.o = !cc foo.cpp\n",
                  .expected = {},
                  .error_lines = {1, 2, 3, 4, 5, 6}},
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
        // A capture in the command makes a pattern rule, whose sources are
        // those of the first word to hold a capture, normalised; the command
        // is kept verbatim, but for what each capture matched, and a `*` that
        // is not a capture is the shell's.
        ParseCase{
            .name = "pattern_run_rule",
            .input = "@/out/$name.o = run cc -c ./src/*{name}.cpp -o %out *\n",
            .expected = {{"out/${name}.o", Action::Run,
                          "cc -c ./src/${name}.cpp -o %out *",
                          "src/*{name}.cpp"}}},
        // A pattern copy's whole path is its pattern.
        ParseCase{
            .name = "pattern_copy_rule",
            .input = "@/$n.txt = copy my notes/note-*{n}.md\n",
            .expected = {{"${n}.txt", Action::Copy, "my notes/note-${n}.md",
                          "my notes/note-*{n}.md"}}},
        // `**` captures, expressions with balanced braces, and `${name}`.
        ParseCase{.name = "pattern_with_every_kind_of_capture",
                  .input = "@/$y/${dir}_x/$n.html = run pandoc "
                           "content/**{dir}/*{y:\\d{4}}-*{n}.md -o %out\n",
                  .expected = {{"${y}/${dir}_x/${n}.html", Action::Run,
                                "pandoc content/${dir}/${y}-${n}.md -o %out",
                                "content/**{dir}/*{y:\\d{4}}-*{n}.md"}}},
        // Without a capture, a `*` in a command is the shell's, as is one
        // not followed by `{`, a name and `}` or `:`.
        ParseCase{
            .name = "a_star_that_is_not_a_capture_is_the_shells",
            .input = "@/all.txt = capture cat src/*.txt\n"
                     "@/b.txt = run cp *{.c,.h} *{a,b} ** dir\n",
            .expected = {{"all.txt", Action::Capture, "cat src/*.txt"},
                         {"b.txt", Action::Run, "cp *{.c,.h} *{a,b} ** dir"}}},
        // The bare `*` wildcard is gone: a `*` in an output path is an
        // error, whatever the command holds.
        ParseCase{.name = "rejects_a_star_in_an_output_path",
                  .input = "@/out/*.o = run cc -c src/*.cpp -o %out\n"
                           "@/out/*{x}.o = run cc -c src/*{x}.cpp\n"
                           "@/*.json = tracing\n",
                  .expected = {},
                  .error_lines = {1, 2, 3}},
        // The source pattern must be an unquoted path inside the manifest's
        // directory, or inside `@/`, keep its captures once normalised, and
        // hold valid expressions.
        ParseCase{.name = "rejects_a_malformed_source_pattern",
                  .input = "@/$x.o = run cc ../*{x}.cpp\n"
                           "@/$x.o = run cc \"src/*{x}.cpp\"\n"
                           "@/$x.o = run cc @/../gen/*{x}.cpp\n"
                           "@/$x.o = run cc src/*{x}/../a.cpp\n"
                           "@/$x.o = copy src/*{x}/\n"
                           "@/$x.o = run cc src/*{x:[}.cpp\n"
                           "@/$x.o = run cc src/*{x:\\d{4}.cpp\n",
                  .expected = {},
                  .error_lines = {1, 2, 3, 4, 5, 6, 7}},
        // Captures go only in the source pattern, each name once, used in
        // the output's path, and not set as well.
        ParseCase{
            .name = "rejects_misplaced_or_unused_captures",
            .input = "@/$x.o = run cc src/*{x}.cpp -o out/*{y}\n"  // 1
                     "@/$x.o = run cc src/*{x}-*{x}.cpp\n"         // 2
                     "@/a.o = run cc src/*{x}.cpp\n"               // 3
                     "@/$x.o = run cc src/*{x}.cpp\n"              // 4
                     "    x = 1\n"
                     "@/$x/$y.o = run cc src/$x/*{y}.cpp\n"  // 6: `$x` unset
                     "@/$y.o = run cc src/*{y}.cpp $x\n"
                     "@/$z.o = run cc src/*{y}.cpp\n",  // 8
            .expected = {{"${y}.o", Action::Run, "cc src/${y}.cpp $x",
                          "src/*{y}.cpp"}},
            .error_lines = {1, 2, 3, 4, 6, 8}},
        // Every `$name` in a path must be captured or set, rule or none, and
        // an output that uses no rule may set variables for its own line.
        ParseCase{.name = "rejects_an_unset_variable_in_a_path",
                  .input = "@/$x.o = run cc src/main.cpp\n"
                           "@/${HOME}/a = run a\n"
                           "@/$lang/index.html = run pandoc -M lang=$lang -o "
                           "%out\n"
                           "    lang = en\n",
                  .expected = {{"en/index.html", Action::Run,
                                "pandoc -M lang=en -o %out"}},
                  .error_lines = {1, 2}},
        // Rules that could produce the same path, depending on the source
        // files there are, are accepted: only the paths they both come to
        // produce fail, which is the build's concern, not the manifest's.
        ParseCase{.name = "accepts_outputs_a_pattern_could_also_produce",
                  .input = "@/out/$n.foo = copy src/*{n}.txt\n"
                           "@/out/a.foo = run a\n"
                           "@/out/lib$n.foo = copy src/*{n}.txt\n"
                           "@/out/x.foo/y = run b\n",
                  .expected = {{"out/${n}.foo", Action::Copy, "src/${n}.txt",
                                "src/*{n}.txt"},
                               {"out/a.foo", Action::Run, "a"},
                               {"out/lib${n}.foo", Action::Copy, "src/${n}.txt",
                                "src/*{n}.txt"},
                               {"out/x.foo/y", Action::Run, "b"}}},
        // Even two patterns with the same output.
        ParseCase{
            .name = "accepts_patterns_sharing_an_output",
            .input = "@/src/$n.js = copy src/*{n}.js\n"
                     "@/src/$n.js = run esbuild src/*{n}.ts --outfile=%out\n",
            .expected =
                {{"src/${n}.js", Action::Copy, "src/${n}.js", "src/*{n}.js"},
                 {"src/${n}.js", Action::Run,
                  "esbuild src/${n}.ts --outfile=%out", "src/*{n}.ts"}}},
        // But a pattern still cannot need a directory where a simple rule
        // declares a file.
        ParseCase{.name = "rejects_a_pattern_inside_a_declared_output",
                  .input = "@/out = run a\n"
                           "@/out/$n.o = copy src/*{n}.c\n",
                  .expected = {{"out", Action::Run, "a"}},
                  .error_lines = {2}},
        // A source pattern under `@/` matches outputs instead of source
        // files, whatever declares them, and is kept without its `@/`.
        ParseCase{
            .name = "pattern_over_outputs",
            .input = "@/gen/$n.md = capture render src/*{n}.txt\n"
                     "@/html/$n.html = run pandoc @/./gen/*{n}.md -o %out\n"
                     "@/site/$n.md = copy @/gen/*{n}.md\n",
            .expected = {{"gen/${n}.md", Action::Capture, "render src/${n}.txt",
                          "src/*{n}.txt"},
                         {"html/${n}.html", Action::Run,
                          "pandoc @/./gen/${n}.md -o %out", "gen/*{n}.md",
                          true},
                         {"site/${n}.md", Action::Copy, "@/gen/${n}.md",
                          "gen/*{n}.md", true}}},
        // A pattern that could match what it produces - itself, or through
        // others - would declare outputs without end.
        ParseCase{.name = "rejects_patterns_that_feed_themselves",
                  .input = "@/gen/$n.md.md = copy @/gen/*{n}.md\n"
                           "@/a/$n.x = copy @/b/*{n}.y\n"
                           "@/b/lib$n.y = copy @/c/*{n}.z\n"
                           "@/c/$n.z = copy @/a/*{n}.x\n"
                           "@/d/$n.x = copy @/a/*{n}.x\n"
                           "@/e/$n.md = copy @/e/sub/*{n}.md\n"
                           "@/f/$n.html = copy @/f/*{n}.md\n"
                           "@/g/x/$n.md = copy @/g/**{n}.md\n"
                           "@/h/$n.txt = copy @/h/**{n}.md\n",
                  .expected = {{"d/${n}.x", Action::Copy, "@/a/${n}.x",
                                "a/*{n}.x", true},
                               {"e/${n}.md", Action::Copy, "@/e/sub/${n}.md",
                                "e/sub/*{n}.md", true},
                               {"f/${n}.html", Action::Copy, "@/f/${n}.md",
                                "f/*{n}.md", true},
                               {"h/${n}.txt", Action::Copy,
                                "@/h/${n}.md", "h/**{n}.md", true}},
                  .error_lines = {1, 2, 3, 4, 8}},
        // An output assigned a `!rule` declared above it takes the rule's
        // action and its body, with `%in` replaced by the rest of the line
        // and each variable by the value set under the output or, failing
        // that, under the rule.
        ParseCase{.name = "uses_a_rule",
                  .input = "!cc = run clang++ $flags -c %in -o %out\n"
                           "    flags = -O2\n"
                           "@/foo.o = !cc foo.cpp\n"
                           "@/bar.o = !cc bar.cpp\n"
                           "    flags = -O0 -g\n",
                  .expected = {{"foo.o", Action::Run,
                                "clang++ -O2 -c foo.cpp -o %out"},
                               {"bar.o", Action::Run,
                                "clang++ -O0 -g -c bar.cpp -o %out"}}},
        // A rule may be a `capture` or a `copy` too, need not take a `%in`,
        // and may leave a variable for each use to set. A copy's path is
        // checked and normalised once it is whole.
        ParseCase{.name = "uses_capture_and_copy_rules",
                  .input = "!describe = capture git describe $what\n"
                           "@/version.txt = !describe\n"
                           "  what = --tags\n"
                           "!publish = copy ./pub/%in\n"
                           "@/a.txt = !publish a.txt\n"
                           "@/b.txt = !publish ../../b.txt\n",
                  .expected = {{"version.txt", Action::Capture,
                                "git describe --tags"},
                               {"a.txt", Action::Copy, "pub/a.txt"}},
                  .error_lines = {6}},
        // The rule is expanded before the output is looked at, so a pattern
        // finds its sources in what `%in` became, and a capture gives a
        // variable of the rule with no default its value - or overrides one.
        ParseCase{.name = "uses_a_rule_for_a_pattern",
                  .input = "!cc = run cc $flags -c %in -o $name.o\n"
                           "@/out/$name.o = !cc src/*{name}.cpp\n"
                           "    flags = -O2\n"
                           "!pandoc = run pandoc %in -M lang=$lang\n"
                           "    lang = en\n"
                           "@/$lang/$n.html = !pandoc posts/*{n}.*{lang}.md\n",
                  .expected = {{"out/${name}.o", Action::Run,
                                "cc -O2 -c src/${name}.cpp -o ${name}.o",
                                "src/*{name}.cpp"},
                               {"${lang}/${n}.html", Action::Run,
                                "pandoc posts/${n}.${lang}.md -M lang=${lang}",
                                "posts/*{n}.*{lang}.md"}}},
        // A capture may come from the rule's own body.
        ParseCase{.name = "takes_a_capture_from_a_rules_body",
                  .input = "!page = run pandoc content/**{path}.md -o %out\n"
                           "@/$path.html = !page\n",
                  .expected = {{"${path}.html", Action::Run,
                                "pandoc content/${path}.md -o %out",
                                "content/**{path}.md"}}},
        // A rule's default is for its body: naming it on an output's line is
        // an error unless the output sets it.
        ParseCase{.name = "rejects_naming_a_rules_default_on_its_line",
                  .input = "!cc = run cc $flags %in\n"
                           "    flags = -O2\n"
                           "@/a.o = !cc a.cpp $flags\n"
                           "@/b.o = !cc b.cpp $flags\n"
                           "    flags = -g\n",
                  .expected = {{"b.o", Action::Run, "cc -g b.cpp -g"}},
                  .error_lines = {3}},
        // `$$` is a `$` for the shell, so `$${z}` is the shell's `${z}`, as
        // is a `$` before anything that cannot start a name. A variable may be
        // set to nothing, and its name ends where the next `$` begins.
        ParseCase{.name = "leaves_other_dollars_for_the_shell",
                  .input = "!r = run echo $$HOME $(date) $1 $x$y $${z} 100$\n"
                           "\tx = a\n"
                           "\ty =\n"
                           "@/o.txt = !r\n",
                  .expected = {{"o.txt", Action::Run,
                                "echo $HOME $(date) $1 a ${z} 100$"}}},
        // What a variable or `%in` is replaced with is not expanded again.
        ParseCase{.name = "does_not_expand_what_it_substitutes",
                  .input = "!r = run tool $a %in\n"
                           "  a = $b %in\n"
                           "@/o.txt = !r $c.txt\n",
                  .expected = {{"o.txt", Action::Run, "tool $b %in $c.txt"}}},
        // Any run of spaces and tabs indents a line, however long or mixed.
        ParseCase{.name = "takes_any_indentation",
                  .input = "!r = run tool $a $b $c $d\n"
                           " a = 1\n"
                           "\tb = 2\n"
                           "\t\t  c = 3\n"
                           "        d = 4\n"
                           "@/o.txt = !r\n",
                  .expected = {{"o.txt", Action::Run, "tool 1 2 3 4"}}},
        // Blank lines and comments do not end the lines under a rule.
        ParseCase{.name = "skips_blanks_and_comments_among_variables",
                  .input = "!r = run tool $a $b\n"
                           "\n"
                           "  # the first\n"
                           "  a = 1\n"
                           "# the second\n"
                           "  b = 2\n"
                           "@/o.txt = !r\n",
                  .expected = {{"o.txt", Action::Run, "tool 1 2"}}},
        // An output that sets and captures nothing expands nothing on its
        // own line: `$`, `$$` and `%in` in its command are the shell's.
        ParseCase{
            .name = "expands_nothing_on_a_line_that_sets_nothing",
            .input = "!r = run tool $a\n"
                     "  a = 1\n"
                     "@/o.txt = run echo $a $$a %in > %out\n",
            .expected = {{"o.txt", Action::Run, "echo $a $$a %in > %out"}}},
        // One that sets or captures something expands only those names, and
        // `$$`, leaving the rest to the shell.
        ParseCase{.name = "expands_only_what_a_line_sets_or_captures",
                  .input = "@/$x.txt = run echo $$HOME $HOME ${x}_draft $y "
                           "src/*{x}.md\n"
                           "@/o.txt = run echo $$a $b ${a}_c\n"
                           "    a = 1\n",
                  .expected = {{"${x}.txt", Action::Run,
                                "echo $HOME $HOME ${x}_draft $y src/${x}.md",
                                "src/*{x}.md"},
                               {"o.txt", Action::Run, "echo $a $b 1_c"}}},
        ParseCase{
            .name = "rejects_misused_rules",
            .input = "@/a = !cc a.cpp\n"  // 1: declared only below
                     "!cc = run cc $flags %in\n"
                     "!cc = run again\n"  // 3: declared twice
                     "!9x = run a\n"      // 4: not a name
                     "!t = tracing\n"     // 5: not an action for a rule
                     "!e = run\n"         // 6: no command
                     "@/b = !cc\n"        // 7: nothing for `%in`
                     "@/c = !cc c.cpp\n"  // 8: `flags` never set
                     "@/d = !cc d.cpp\n"
                     "  flags = -O2\n"
                     "  other = 1\n"  // 11: not one of its variables
                     "@/e = !cc e.cpp\n"
                     "  flags = 1\n"
                     "  flags = 2\n"  // 14: set twice
                     "  nonsense\n"   // 15: not `name = value`
                     "@/f = run f\n"
                     "  flags = 1\n"  // 17: not used on its line
                     "!n = capture date\n"
                     "@/g = !n arg\n"  // 19: no `%in` to take it
                     "!u = run $a\n"
                     "  b = 1\n"   // 21: a default it never uses
                     "@/h = !u\n"  // 22: so `!u` was not declared
                     "@/i = !cc i.cpp\n"
                     "  flags = -O1\n",
            .expected = {{"i", Action::Run, "cc -O1 i.cpp"}},
            .error_lines = {1, 3, 4, 5, 6, 7, 8, 11, 14, 15, 17, 19, 21, 22}},
        // An indented line belongs to the line above it, so one with nothing
        // above it belongs nowhere.
        ParseCase{.name = "rejects_an_indented_first_line",
                  .input = "  @/a.txt = run a\n"
                           "@/b.txt = run b\n",
                  .expected = {{"b.txt", Action::Run, "b"}},
                  .error_lines = {1}},
        ParseCase{
            .name = "handles_a_final_line_without_a_newline",
            .input = "@/output.txt = run cp input.txt %out",
            .expected = {{"output.txt", Action::Run, "cp input.txt %out"}}}),
    [](const ::testing::TestParamInfo<ParseCase>& info) {
      return std::string(info.param.name);
    });

// Each mistake with captures and variables is explained, so whoever wrote
// it can tell what to do instead.
TEST(ManifestParse, ExplainsEachMistake) {
  const std::vector<std::pair<std::string_view, std::string_view>> cases = {
      {"@/out/*.o = run cc -c src/*.cpp -o %out\n",
       "capture part of its source pattern with `*{name}` and use it in the "
       "path as `$name`"},
      {"@/$x.o = run cc src/*{x}.cpp -o *{y}\n", "outside its source pattern"},
      {"@/$x.o = run cc src/*{x}-*{x}.cpp\n", "captures `$x` twice"},
      {"@/a.o = run cc src/*{x}.cpp\n", "captures `$x` but never uses it"},
      {"@/$x.o = run cc src/*{x}.cpp\n    x = 1\n",
       "both captures and sets `$x`"},
      {"@/$x.o = run cc src/main.cpp\n", "neither captures nor sets"},
      {"!cc = run cc $flags %in\n    flags = -O2\n@/a.o = !cc a.cpp $flags\n",
       "for the rule's body alone"},
      {"!cc = run cc $flags %in\n@/a.o = !cc a.cpp\n", "has no default"},
      {"@/o.txt = run echo\n    x = 1\n", "which its line never uses"},
      {"@/$x.o = run cc \"src/*{x}.cpp\"\n", "must be an unquoted path"},
      {"@/$x.o = run cc src/*{x:[}.cpp\n", "not a valid ECMAScript"},
      {"@/$x.o = run cc src/*{x:\\d{4}.cpp\n", "has no `}` to end it"},
      {"@/$y.o = copy src/$y/*{y}.c\n",
       "uses a variable it captures in its source pattern"},
      {"@/gen/$n.md.md = copy @/gen/*{n}.md\n", "would never end"},
  };
  for (const auto& [input, message] : cases) {
    const makebelieve::Manifest manifest = makebelieve::Manifest::parse(input);
    ASSERT_EQ(manifest.errors().size(), 1U) << input;
    EXPECT_TRUE(manifest.errors()[0].message.contains(message))
        << input << manifest.errors()[0].message;
    EXPECT_TRUE(manifest.rules().empty()) << input;
  }
}

// The rule @a manifest's only rule stands for given @a path, or nullopt.
std::optional<makebelieve::Manifest::Rule> instantiate(
    std::string_view manifest,
    std::string_view path) {
  const makebelieve::Manifest parsed = makebelieve::Manifest::parse(manifest);
  EXPECT_TRUE(parsed.errors().empty()) << manifest;
  EXPECT_EQ(parsed.rules().size(), 1U) << manifest;
  if (parsed.rules().size() != 1U) {
    return std::nullopt;
  }
  EXPECT_TRUE(parsed.rules()[0].is_pattern());
  return makebelieve::Manifest::instantiate(parsed.rules()[0], path);
}

using Made = std::pair<std::string, std::string>;

// The output and command @a manifest's only rule makes of @a path, or empty
// strings if it does not match.
Made made(std::string_view manifest, std::string_view path) {
  const auto rule = instantiate(manifest, path);
  if (!rule.has_value()) {
    return {};
  }
  EXPECT_FALSE(rule->is_pattern());
  return {rule->output.generic_string(), rule->command};
}

// A pattern rule stands for one simple rule per matching path, with what each
// capture matched put into the output and the command.
TEST(ManifestInstantiate, SubstitutesEachCapture) {
  const makebelieve::Manifest manifest = makebelieve::Manifest::parse(
      "# objects\n"
      "@/out/lib$name.o = run cc -c src/*{name}.cpp -MF $name.d -o %out\n");
  ASSERT_EQ(manifest.rules().size(), 1U);
  const makebelieve::Manifest::Rule& rule = manifest.rules()[0];
  ASSERT_TRUE(rule.is_pattern());
  EXPECT_EQ(rule.line, 2U);
  EXPECT_EQ(rule.text,
            "@/out/lib$name.o = run cc -c src/*{name}.cpp -MF $name.d -o "
            "%out");
  EXPECT_EQ(rule.pattern->directory, std::filesystem::path("src"));
  EXPECT_FALSE(rule.pattern->recursive);

  const auto matched = makebelieve::Manifest::instantiate(rule, "src/main.cpp");
  ASSERT_TRUE(matched.has_value());
  EXPECT_EQ(matched->output, std::filesystem::path("out") / "libmain.o");
  EXPECT_EQ(matched->action, Action::Run);
  EXPECT_EQ(matched->command, "cc -c src/main.cpp -MF main.d -o %out");
  EXPECT_FALSE(matched->is_pattern());
  // It keeps the line, and the text, of the rule it stands for.
  EXPECT_EQ(matched->line, 2U);
  EXPECT_EQ(matched->text, rule.text);

  // A capture matches at least one character, never a `/`, and the whole
  // path must match.
  for (const std::string_view path :
       {"src/.cpp", "src/main.c", "src/main.cpp.bak", "src/a/b.cpp", "main.cpp",
        "x/src/main.cpp"}) {
    EXPECT_FALSE(makebelieve::Manifest::instantiate(rule, path).has_value())
        << path;
  }
}

// Captures can lay a path out differently, each taking as little as it can
// from the left.
TEST(ManifestInstantiate, LaysOutADatedPost) {
  constexpr std::string_view manifest =
      "@/$year/$month/$day/$name.html = run pandoc "
      "posts/*{year}-*{month}-*{day}-*{name}.md -o %out\n";
  EXPECT_EQ(made(manifest, "posts/2024-01-02-my-post.md"),
            (Made{"2024/01/02/my-post.html",
                  "pandoc posts/2024-01-02-my-post.md -o %out"}));
  EXPECT_EQ(made(manifest, "posts/about-us-page-v2.md"),
            (Made{"about/us/page/v2.html",
                  "pandoc posts/about-us-page-v2.md -o %out"}));
  EXPECT_EQ(made(manifest, "posts/about-us.md"), Made{});
}

// An expression captures only what it matches in full; a path no split
// matches is skipped.
TEST(ManifestInstantiate, HoldsCapturesToTheirExpressions) {
  constexpr std::string_view manifest =
      "@/$year/$month/$day/$name.html = run pandoc "
      "posts/*{year:\\d{4}}-*{month:\\d\\d}-*{day:\\d\\d}-*{name}.md -o "
      "%out\n";
  EXPECT_EQ(made(manifest, "posts/2024-01-02-my-post.md"),
            (Made{"2024/01/02/my-post.html",
                  "pandoc posts/2024-01-02-my-post.md -o %out"}));
  EXPECT_EQ(made(manifest, "posts/about-us-page-v2.md"), Made{});
  EXPECT_EQ(made(manifest, "posts/24-01-02-x.md"), Made{});
}

// `**` reaches into subdirectories, and `*` never holds a `/`, so a path can
// be taken apart into its directory and its file name.
TEST(ManifestInstantiate, TakesADirectoryAndAFileName) {
  constexpr std::string_view split =
      "@/$dir/index/$name.html = copy content/**{dir}/*{name}.md\n";
  EXPECT_EQ(made(split, "content/a/b/c.md"),
            (Made{"a/b/index/c.html", "content/a/b/c.md"}));
  EXPECT_EQ(made(split, "content/a/c.md"),
            (Made{"a/index/c.html", "content/a/c.md"}));
  EXPECT_EQ(made(split, "content/c.md"), Made{});

  constexpr std::string_view whole =
      "@/$path.html = run pandoc content/**{path}.md -o %out\n";
  EXPECT_EQ(made(whole, "content/blog/2024/intro.md"),
            (Made{"blog/2024/intro.html",
                  "pandoc content/blog/2024/intro.md -o %out"}));

  for (const std::string_view manifest : {split, whole}) {
    const makebelieve::Manifest parsed = makebelieve::Manifest::parse(manifest);
    ASSERT_EQ(parsed.rules().size(), 1U);
    EXPECT_EQ(parsed.rules()[0].pattern->directory,
              std::filesystem::path("content"));
    EXPECT_TRUE(parsed.rules()[0].pattern->recursive);
  }
}

// An expression's own quantifiers decide how much it takes, but a `*{}` never
// holds a `/` whatever its expression allows, and a back reference counts
// within its own expression.
TEST(ManifestInstantiate, LetsAnExpressionDecideHowMuchItTakes) {
  EXPECT_EQ(made("@/$a/$b = copy src/**{a:.+}-**{b}\n", "src/x-y-z"),
            (Made{"x-y/z", "src/x-y-z"}));
  EXPECT_EQ(made("@/$a/$b = copy src/**{a}-**{b}\n", "src/x-y-z"),
            (Made{"x/y-z", "src/x-y-z"}));
  EXPECT_EQ(made("@/$x.txt = copy src/*{x:.+}.md\n", "src/a/b.md"), Made{});
  EXPECT_EQ(made("@/$x.txt = copy src/*{x:[\\s\\S]+}.md\n", "src/a/b.md"),
            Made{});
  EXPECT_EQ(made("@/$x.txt = copy src/*{x:.+}.md\n", "src/a.b.md"),
            (Made{"a.b.txt", "src/a.b.md"}));
  constexpr std::string_view repeated =
      "@/$d/$x.txt = copy src/*{d:(\\w)\\1}-*{x:(\\w)\\1}.txt\n";
  EXPECT_EQ(made(repeated, "src/aa-bb.txt"),
            (Made{"aa/bb.txt", "src/aa-bb.txt"}));
  EXPECT_EQ(made(repeated, "src/aa-bc.txt"), Made{});
  EXPECT_EQ(made(repeated, "src/ab-bb.txt"), Made{});
}

// Captures go into a rule's body, and literal text in the pattern matches
// only itself.
TEST(ManifestInstantiate, FillsARulesBody) {
  constexpr std::string_view manifest =
      "!post = run pandoc -M date=$year-$month-$day %in -o %out\n"
      "@/$year/$month/$day/$name.html = !post "
      "posts/*{year}-*{month}-*{day}-*{name}.md\n";
  EXPECT_EQ(made(manifest, "posts/2024-01-02-my-post.md"),
            (Made{"2024/01/02/my-post.html",
                  "pandoc -M date=2024-01-02 posts/2024-01-02-my-post.md -o "
                  "%out"}));
  EXPECT_EQ(made("@/$x = copy a.b(c)/*{x}\n", "a.b(c)/d"),
            (Made{"d", "a.b(c)/d"}));
  EXPECT_EQ(made("@/$x = copy a.b(c)/*{x}\n", "aXb(c)/d"), Made{});
}

// A pattern over outputs matches paths beneath `@/`, and its command keeps
// the `@/` that names the output.
TEST(ManifestInstantiate, MatchesOutputs) {
  EXPECT_EQ(
      made("@/site/$n.html = run pandoc @/gen/*{n}.md -o %out\n", "gen/a.md"),
      (Made{"site/a.html", "pandoc @/gen/a.md -o %out"}));
  EXPECT_EQ(made("@/site/$n.md = copy @/gen/*{n}.md\n", "gen/a.md"),
            (Made{"site/a.md", "@/gen/a.md"}));
}

}  // namespace
