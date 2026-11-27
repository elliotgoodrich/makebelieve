// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace makebelieve {

/// @class Manifest
/// The parsed contents of a `build.makebelieve` file: the ordered set of
/// output rules it declares, and any lines it could not accept.
class Manifest {
 public:
  /// Where a rule's output comes from.
  enum class Action {
    /// `run <command>`: the command writes the output itself, to the path
    /// `%out` stands for.
    Run,
    /// `capture <command>`: whatever the command writes to standard output is
    /// the output.
    Capture,
    /// `copy <path>`: the output is the bytes of the file at `<path>`. No
    /// command runs, and `<path>` - a source file, or an output under `@/` -
    /// is the rule's one input.
    Copy,
    /// `tracing`: the output is a trace of what makebelieve has been doing, in
    /// the Trace Event Format. Takes no argument.
    Tracing,
    /// `status`: the output is a report of what makebelieve is doing and what
    /// went wrong - each output building or waiting to, each whose last build
    /// failed, and why a change to the manifest was rejected - empty once
    /// everything is built. Takes no argument.
    Status,
  };

  /// Whether @a action is one whose output reports on makebelieve itself -
  /// `Tracing` or `Status` - rather than being built: it takes no argument,
  /// cannot have captures, and is served by the tree itself.
  [[nodiscard]] static constexpr bool is_report(Action action) {
    return action == Action::Tracing || action == Action::Status;
  }

  /// Text with what a pattern rule's captures matched to put in: literal
  /// pieces and, between each pair, the value of one capture.
  struct Template {
    /// One more than @link captures, the text around each capture.
    std::vector<std::string> literals;
    /// The index, into Pattern::names, of each capture put in.
    std::vector<std::size_t> captures;

    /// This text with each capture's value from @a values, indexed as
    /// Pattern::names is.
    [[nodiscard]] std::string fill(std::span<const std::string> values) const;

    /// This text with each capture as `${name}`, for showing it.
    [[nodiscard]] std::string spelled(std::span<const std::string> names) const;
  };

  /// What a rule with captures matches, and what it makes of each match.
  struct Pattern {
    /// The source pattern, normalised and without any `@/`, its captures as
    /// written: `posts/*{year:\d{4}}-*{name}.md`.
    std::string source;

    /// Whether it was written under `@/` and so matches declared outputs,
    /// rather than files in the manifest's directory.
    bool over_outputs = false;

    /// The name of each capture, in the order they appear.
    std::vector<std::string> names;

    /// The literal text of @link source around its captures, normalised.
    Template pieces;

    /// The directory holding the literal start of @link source, which the
    /// files it matches are in or beneath, and whether they may be beneath
    /// it - a `**` capture, or a `/` after a capture - so finding them lists
    /// every directory beneath it.
    std::filesystem::path directory;
    bool recursive = false;

    /// The output path and the command, or for `Copy` the path, to fill in.
    Template output;
    Template command;

    /// What @link source matches: the whole of a path relative to the
    /// manifest's directory, or to `@/`, with each capture in its group of
    /// @link groups.
    std::regex regex;
    std::vector<std::size_t> groups;

    /// For each capture, whether it may hold a `/`: a `**` capture.
    std::vector<bool> crosses_directories;
  };

  /// A single `@/output = <action> <argument>` rule: the output path with its
  /// leading `@/` stripped, the action that says where its content comes from,
  /// and that action's argument. A pattern rule, whose source pattern holds a
  /// capture, stands for one such rule per path @link Pattern::regex matches;
  /// @link instantiate makes each.
  struct Rule {
    /// The output; for a pattern rule, its path with each capture spelled
    /// `${name}`.
    std::filesystem::path output;
    Action action;

    /// The command to run for `Run` and `Capture`, taken verbatim; for `Copy`,
    /// the normalised path of the file to copy, relative to the manifest's
    /// directory, or `@/` and the output to copy; empty for `Tracing` and
    /// `Status`. For a pattern rule, each capture is spelled `${name}`.
    std::string command;

    /// The manifest line declaring the rule, as written.
    std::string text;

    /// What a pattern rule matches, or null for a simple rule.
    std::shared_ptr<const Pattern> pattern;

    /// The 1-based line of the manifest that declared the rule, or of the
    /// pattern rule it was instantiated from.
    std::size_t line = 0;

    [[nodiscard]] bool is_pattern() const { return pattern != nullptr; }
  };

  /// A line that could not be accepted: its 1-based number and why.
  struct Error {
    std::size_t line;
    std::string message;
  };

  /// Parses @a text into a `Manifest`. Blank lines and `#` comments are
  /// skipped. Every other line that is not indented must be a simple
  /// `@/output = run <command>`, `@/output = capture <command>`,
  /// `@/output = copy <path>`, `@/output = tracing` or `@/output = status`
  /// rule, or the use or declaration of a named rule (below), naming a file
  /// inside the output directory that no earlier rule declared, either as that
  /// file or as one of its parent directories. A `copy` rule must name a file
  /// inside the manifest's directory, so its source is always one the build
  /// can watch, or another declared output.
  ///
  /// `!name = <action> <body>`, with `run`, `capture` or `copy` for its
  /// action, declares a named rule, which yields nothing itself. An output
  /// below it written `@/output = !name <argument>` is the rule
  /// `@/output = <action> <body>`, the body having each `%in` replaced by the
  /// argument and each `$variable` or `${variable}` by its value, and is then
  /// held to everything a rule written out in full is. The indented
  /// `variable = value` lines under the output set its variables, and those
  /// under the declaration their defaults. `$$` in a body is a `$`, and a `$`
  /// before anything that cannot start a name is left alone. What is
  /// substituted in is not expanded again. An output's own line - its path
  /// and what follows the `=` - expands only the variables set under it and
  /// the names it captures, and leaves everything else, `$$` included when it
  /// sets and captures nothing, as it is.
  ///
  /// The first word after any rule is expanded - or a `copy`'s whole path -
  /// to hold a capture, `*{name}`, `**{name}`, `*{name:regex}` or
  /// `**{name:regex}`, is the source pattern of a pattern rule, which names
  /// source files inside the manifest's directory or, under `@/`, other
  /// outputs. A capture anywhere else is an error, as is a `*` in an output
  /// path, capturing a name twice or one that is set, not using a capture in
  /// the output path, a `$name` in the path that is neither captured nor set,
  /// and a rule's default named on an output's line that does not set it.
  /// A pattern rule may produce a path that another rule declares too,
  /// depending on what source files exist; that is not decided here but by
  /// whatever matches the patterns, which fails that one path. A pattern over
  /// outputs may not be able to produce, directly or through other patterns,
  /// an output its own pattern matches, or its outputs would never end.
  ///
  /// Each line that breaks these is reported in @link errors and left out of
  /// @link rules.
  [[nodiscard]] static Manifest parse(std::string_view text);

  /// The simple rule the pattern rule @a rule stands for given the path
  /// @a path - relative to the manifest's directory, or for a pattern over
  /// outputs to `@/` - with each capture's value put into its output and
  /// command, or nullopt if @a path does not match or makes no valid output.
  /// @pre `rule.is_pattern()`
  [[nodiscard]] static std::optional<Rule> instantiate(const Rule& rule,
                                                       std::string_view path);

  /// The rules the manifest declares, in the order they appeared.
  [[nodiscard]] std::span<const Rule> rules() const { return m_rules; }

  /// The lines that could not be accepted, in the order they appeared.
  [[nodiscard]] std::span<const Error> errors() const { return m_errors; }

 private:
  std::vector<Rule> m_rules;
  std::vector<Error> m_errors;
};

}  // namespace makebelieve
