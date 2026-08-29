// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace makebelieve {

/// @class Manifest
/// The parsed contents of a `build.makebelieve` file: the ordered set of
/// output rules it declares.
class Manifest {
 public:
  /// A single simple `@/output <- command` rule: the output path with its
  /// leading `@/` stripped, and the command that produces it.
  struct Rule {
    std::filesystem::path output;
    std::string command;
  };

  /// Parses @a text into a `Manifest`. Blank lines and `#` comments are
  /// skipped, as is any line that is not a simple `@/output <- command` rule -
  /// the richer manifest features (variables, `rule` declarations, wildcards,
  /// placeholders) are not handled yet.
  [[nodiscard]] static Manifest parse(std::string_view text);

  /// The rules the manifest declares, in the order they appeared.
  [[nodiscard]] std::span<const Rule> rules() const { return m_rules; }

 private:
  std::vector<Rule> m_rules;
};

}  // namespace makebelieve
