// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
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
  };

  /// A single `@/output = <action> <argument>` rule: the output path with its
  /// leading `@/` stripped, the action that says where its content comes from,
  /// and that action's argument. A wildcard rule, whose output's file name
  /// holds a `*`, stands for one such rule per source file matching
  /// @link wildcard_source; @link instantiate makes each.
  struct Rule {
    std::filesystem::path output;
    Action action;

    /// The command to run for `Run` and `Capture`, taken verbatim; for `Copy`,
    /// the normalised path of the file to copy, relative to the manifest's
    /// directory, or `@/` and the output to copy; empty for `Tracing`.
    std::string command;

    /// For a wildcard rule, the normalised pattern its sources match, with a
    /// single `*` in its file name: the first word of the command to hold a
    /// `*`, or a `Copy`'s whole path. Empty for a simple rule.
    std::filesystem::path wildcard_source;

    /// Whether @link wildcard_source was written under `@/` and so matches
    /// declared outputs, rather than files in the manifest's directory.
    bool wildcard_over_outputs = false;

    [[nodiscard]] bool is_wildcard() const { return !wildcard_source.empty(); }
  };

  /// A line that could not be accepted: its 1-based number and why.
  struct Error {
    std::size_t line;
    std::string message;
  };

  /// Parses @a text into a `Manifest`. Blank lines and `#` comments are
  /// skipped. Every other line must be a simple `@/output = run <command>`,
  /// `@/output = capture <command>`, `@/output = copy <path>` or
  /// `@/output = tracing` rule - the
  /// richer manifest features (variables, `rule` declarations,
  /// placeholders) are not handled yet - naming a file inside the output
  /// directory that no earlier rule declared, either as that file or as one of
  /// its parent directories. A `copy` rule must name a file inside the
  /// manifest's directory, so its source is always one the build can watch,
  /// or another declared output.
  ///
  /// An output with a `*` makes a `run`, `capture` or `copy` rule a wildcard
  /// rule. The `*` must be the only one and in the file name, and so must the
  /// one in its source pattern, which names source files inside the manifest's
  /// directory or, under `@/`, other outputs. No two rules may be able to
  /// produce the same path whatever source files come to exist, so a wildcard
  /// bans its suffix - whatever follows its `*` - from its directory: an
  /// output there, or a directory of one, ending in that suffix is rejected,
  /// as is another wildcard there if either's suffix ends in the other's. Nor
  /// may a wildcard over outputs be able to produce, directly or through other
  /// wildcards, an output its own pattern matches.
  ///
  /// Each line that breaks these is reported in @link errors and left out of
  /// @link rules.
  [[nodiscard]] static Manifest parse(std::string_view text);

  /// The simple rule the wildcard rule @a rule stands for given a source
  /// named @a source_name in the directory of its source pattern - the output
  /// and every `*` of the command taking whatever the pattern's `*` matched -
  /// or nullopt if the name does not match. A `*` matches one or more
  /// characters.
  /// @pre `rule.is_wildcard()`
  [[nodiscard]] static std::optional<Rule> instantiate(
      const Rule& rule,
      std::string_view source_name);

  /// The rules the manifest declares, in the order they appeared.
  [[nodiscard]] std::span<const Rule> rules() const { return m_rules; }

  /// The lines that could not be accepted, in the order they appeared.
  [[nodiscard]] std::span<const Error> errors() const { return m_errors; }

 private:
  std::vector<Rule> m_rules;
  std::vector<Error> m_errors;
};

}  // namespace makebelieve
