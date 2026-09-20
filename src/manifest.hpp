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
  /// How a rule collects the output of the command it names.
  enum class Action {
    /// `run <command>`: the command writes the output itself, to the path
    /// `%out` stands for.
    Run,
    /// `capture <command>`: whatever the command writes to standard output is
    /// the output.
    Capture,
  };

  /// A single simple `@/output = <action> <command>` rule: the output path
  /// with its leading `@/` stripped, and the command that produces it with the
  /// action that collects its result.
  struct Rule {
    std::filesystem::path output;
    Action action;
    std::string command;
  };

  /// A line that could not be accepted: its 1-based number and why.
  struct Error {
    std::size_t line;
    std::string message;
  };

  /// Parses @a text into a `Manifest`. Blank lines and `#` comments are
  /// skipped. Every other line must be a simple `@/output = run <command>` or
  /// `@/output = capture <command>` rule - the richer manifest features
  /// (variables, `rule` declarations, wildcards, placeholders) are not handled
  /// yet - naming a file inside the output directory that no earlier rule
  /// declared, either as that file or as one of its parent directories. Each
  /// line that is not is reported in @link errors and left out of @link
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
