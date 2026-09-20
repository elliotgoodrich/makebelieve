// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <filesystem>
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
    /// command runs, and `<path>` is the rule's one input.
    Copy,
  };

  /// A single simple `@/output = <action> <argument>` rule: the output path
  /// with its leading `@/` stripped, the action that says where its content
  /// comes from, and that action's argument.
  struct Rule {
    std::filesystem::path output;
    Action action;

    /// The command to run for `Run` and `Capture`, taken verbatim; for `Copy`,
    /// the normalised path of the file to copy, relative to the manifest's
    /// directory.
    std::string command;
  };

  /// A line that could not be accepted: its 1-based number and why.
  struct Error {
    std::size_t line;
    std::string message;
  };

  /// Parses @a text into a `Manifest`. Blank lines and `#` comments are
  /// skipped. Every other line must be a simple `@/output = run <command>`,
  /// `@/output = capture <command>` or `@/output = copy <path>` rule - the
  /// richer manifest features (variables, `rule` declarations, wildcards,
  /// placeholders) are not handled yet - naming a file inside the output
  /// directory that no earlier rule declared, either as that file or as one of
  /// its parent directories. A `copy` rule must name a file inside the
  /// manifest's directory, so its source is always one the build can watch.
  /// Each line that is not is reported in @link errors and left out of @link
  /// rules.
  [[nodiscard]] static Manifest parse(std::string_view text);

  /// The rules the manifest declares, in the order they appeared.
  [[nodiscard]] std::span<const Rule> rules() const { return m_rules; }

  /// The lines that could not be accepted, in the order they appeared.
  [[nodiscard]] std::span<const Error> errors() const { return m_errors; }

 private:
  std::vector<Rule> m_rules;
  std::vector<Error> m_errors;
};

}  // namespace makebelieve
