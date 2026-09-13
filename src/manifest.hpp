// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
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
    /// Resolved output > named rule > filename selector, relative to source.
    std::optional<std::filesystem::path> placeholder;
  };

  /// Parses @a text into a `Manifest`. Blank lines and `#` comments are
  /// skipped. Supports named commands with `%in` and indented `%placeholder`
  /// settings on outputs, named rules and filename selectors. Variables and
  /// wildcard output expansion are not handled yet - a variable setting inside
  /// a block is accepted and ignored rather than ending the block.
  ///
  /// Throws `std::invalid_argument` for syntax the parser understands but
  /// cannot honour: a malformed declaration, a setting outside any block, a
  /// selector with more than one `*`, or an invocation of an undeclared rule.
  [[nodiscard]] static Manifest parse(std::string_view text);

  /// Replaces every standalone `%`-token @a name in @a text with @a value.
  /// @a name includes its leading `%`. A token immediately followed by another
  /// name character is part of a longer one (`%output` is not `%out`) and is
  /// left as it stands, so a short name never corrupts a long one.
  [[nodiscard]] static std::string substitute(std::string_view text,
                                              std::string_view name,
                                              std::string_view value);

  /// The rules the manifest declares, in the order they appeared.
  [[nodiscard]] std::span<const Rule> rules() const { return m_rules; }

 private:
  std::vector<Rule> m_rules;
};

}  // namespace makebelieve
