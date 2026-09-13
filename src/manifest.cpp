// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace makebelieve {

namespace {

std::string_view trim(std::string_view text) {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// Characters that continue a `%`-token or a rule name. A name followed by one
// of these is the prefix of a longer name, so both substitution and invocation
// stop at the first character that is not one.
bool is_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

// Strips one layer of matching quotes from a setting's value, so a path
// containing spaces can be written `%placeholder = "loading page.html"`.
std::string_view unquote(std::string_view value) {
  const bool quoted = value.size() >= 2 &&
                      (value.front() == '"' || value.front() == '\'') &&
                      value.back() == value.front();
  return quoted ? value.substr(1, value.size() - 2) : value;
}

// A `[selector]` block: the filename pattern it selects on, and the placeholder
// every output matching it inherits.
struct Selector {
  std::string name;
  std::optional<std::filesystem::path> placeholder;
};

// A `rule name = command` declaration and the settings indented beneath it.
struct NamedRule {
  std::string command;
  std::optional<std::filesystem::path> placeholder;
};

using NamedRules = std::map<std::string, NamedRule, std::less<>>;

// True when @a line opens a new top-level item rather than continuing the block
// above it. Tested before a block's settings so an indented declaration closes
// its block instead of being swallowed as a setting - a command such as
// `build --mode=fast` is otherwise indistinguishable from one.
bool opens_top_level(std::string_view line) {
  return (line.front() == '[' && line.back() == ']') ||
         line.starts_with("rule ") ||
         (line.starts_with("@/") && line.find("<-") != std::string_view::npos);
}

// The two halves of a `name: input` rule invocation.
struct Invocation {
  std::string_view name;
  std::string_view input;
};

// Splits @a command into a rule invocation, or nullopt when it is an ordinary
// shell command. An invocation is a bare identifier, a colon, then whitespace
// or the end of the line - a shape no path or URL can take, so
// `C:\tools\build.exe`, `curl https://example.com` and
// `powershell -c "echo hi" : 2` all stay commands while `cc: foo.cpp` invokes
// a rule. The test is deliberately independent of which rules are declared:
// whether a command runs through the shell must not change as rules are added.
std::optional<Invocation> split_invocation(std::string_view command) {
  const std::size_t colon = command.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view rest = command.substr(colon + 1);
  if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t') {
    return std::nullopt;
  }
  const std::string_view name = trim(command.substr(0, colon));
  if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
    return std::nullopt;
  }
  for (const char c : name) {
    if (!is_name_char(c) && c != '-') {
      return std::nullopt;
    }
  }
  return Invocation{.name = name, .input = trim(rest)};
}

// Quotes @a input when it needs it, so a `%in` path containing spaces survives
// the shell the way the quoted `%out` path does. Input that is already quoted,
// or that has no whitespace to protect, is passed through untouched - a
// wildcard the shell is meant to expand must not be quoted.
std::string quote_input(std::string_view input) {
  if (input != unquote(input) ||
      input.find_first_of(" \t") == std::string_view::npos) {
    return std::string(input);
  }
  return "\"" + std::string(input) + "\"";
}

// How specific a selector's match is. A more specific selector wins outright;
// among equally specific ones the last declaration wins.
enum class Specificity : std::uint8_t { none, any, pattern, exact };

Specificity match(const Selector& selector,
                  const std::filesystem::path& output) {
  if (selector.name == "*") {
    return Specificity::any;
  }
  if (selector.name == "no extension") {
    return output.extension().empty() ? Specificity::pattern
                                      : Specificity::none;
  }
  const std::string filename = output.filename().string();
  const std::size_t star = selector.name.find('*');
  if (star == std::string::npos) {
    return filename == selector.name ? Specificity::exact : Specificity::none;
  }
  const std::string_view pattern(selector.name);
  const std::string_view prefix = pattern.substr(0, star);
  const std::string_view suffix = pattern.substr(star + 1);
  // The size check stops `*` matching the same characters twice, so the
  // pattern `a*a` does not match the single-character filename `a`.
  const bool matches = filename.size() >= prefix.size() + suffix.size() &&
                       filename.starts_with(prefix) &&
                       filename.ends_with(suffix);
  return matches ? Specificity::pattern : Specificity::none;
}

// Rejected at the declaration rather than left to silently never match.
void validate_selector(std::string_view name) {
  if (name.empty()) {
    throw std::invalid_argument("a selector must name a filename or pattern");
  }
  const std::size_t star = name.find('*');
  if (star != std::string_view::npos &&
      name.find('*', star + 1) != std::string_view::npos) {
    throw std::invalid_argument("a selector supports at most one `*`: [" +
                                std::string(name) + "]");
  }
}

}  // namespace

std::string Manifest::substitute(std::string_view text,
                                 std::string_view name,
                                 std::string_view value) {
  std::string result;
  std::size_t pos = 0;
  while (true) {
    const std::size_t found = text.find(name, pos);
    if (found == std::string_view::npos) {
      result += text.substr(pos);
      return result;
    }
    const std::size_t after = found + name.size();
    result += text.substr(pos, found - pos);
    // `%output` merely starts with `%out`; leave the longer name as it stands.
    result += after < text.size() && is_name_char(text[after]) ? name : value;
    pos = after;
  }
}

Manifest Manifest::parse(std::string_view text) {
  Manifest manifest;
  // Deques, so the pointer a block writes its settings through stays valid as
  // later declarations are appended.
  std::deque<Selector> selectors;
  std::deque<Rule> rules;
  NamedRules named;

  // The placeholder the current block's settings apply to, and the indent of
  // the line that opened it. Null between blocks.
  std::optional<std::filesystem::path>* block_placeholder = nullptr;
  std::size_t block_indent = 0;

  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end =
        newline == std::string_view::npos ? text.size() : newline;
    const std::string_view raw = text.substr(start, end - start);
    const std::string_view line = trim(raw);
    start = end + 1;

    if (line.empty() || line.front() == '#') {
      continue;  // Blank lines and comments do not close a block.
    }
    // The line has a non-blank character, since trimming left something.
    const std::size_t indent = raw.find_first_not_of(" \t");

    if (block_placeholder != nullptr && indent > block_indent &&
        !opens_top_level(line)) {
      const std::size_t equals = line.find('=');
      if (equals == std::string_view::npos) {
        throw std::invalid_argument(
            "expected `setting = value` inside an indented block: " +
            std::string(line));
      }
      // `%placeholder` is the only setting implemented. The manifest's other
      // settings (variables) are accepted and ignored here rather than closing
      // the block, so a `%placeholder` written below one still applies.
      if (trim(line.substr(0, equals)) == "%placeholder") {
        const std::string_view path = unquote(trim(line.substr(equals + 1)));
        if (path.empty()) {
          throw std::invalid_argument(
              "%placeholder requires a source file path");
        }
        *block_placeholder = std::filesystem::path(path).lexically_normal();
      }
      continue;
    }
    block_placeholder = nullptr;
    block_indent = indent;

    if (line.front() == '%') {
      throw std::invalid_argument(
          "a setting must be indented under the output, rule or selector it "
          "applies to: " +
          std::string(line));
    }

    if (line.front() == '[' && line.back() == ']') {
      const std::string_view name = trim(line.substr(1, line.size() - 2));
      validate_selector(name);
      selectors.push_back({.name = std::string(name)});
      block_placeholder = &selectors.back().placeholder;
      continue;
    }

    if (line.starts_with("rule ")) {
      const std::size_t equals = line.find('=');
      const std::string_view name = equals == std::string_view::npos
                                        ? std::string_view()
                                        : trim(line.substr(5, equals - 5));
      const std::string_view command = equals == std::string_view::npos
                                           ? std::string_view()
                                           : trim(line.substr(equals + 1));
      if (name.empty() || command.empty()) {
        throw std::invalid_argument("expected `rule <name> = <command>`: " +
                                    std::string(line));
      }
      NamedRule& rule = named[std::string(name)];
      rule = {.command = std::string(command)};
      block_placeholder = &rule.placeholder;
      continue;
    }

    const std::size_t arrow = line.find("<-");
    if (arrow == std::string_view::npos) {
      continue;
    }

    const std::string_view left = trim(line.substr(0, arrow));
    const std::string_view command = trim(line.substr(arrow + 2));
    if (!left.starts_with("@/") || command.empty()) {
      continue;
    }

    const std::string_view output = left.substr(2);
    if (output.empty()) {
      continue;
    }

    rules.push_back({.output = std::filesystem::path(output).lexically_normal(),
                     .command = std::string(command)});
    block_placeholder = &rules.back().placeholder;
  }

  // Resolve after parsing so declaration order does not affect inheritance.
  for (Rule& rule : rules) {
    if (const std::optional<Invocation> call = split_invocation(rule.command)) {
      const auto it = named.find(call->name);
      if (it == named.end()) {
        throw std::invalid_argument("no `rule " + std::string(call->name) +
                                    "` is declared: " + rule.command);
      }
      rule.command =
          substitute(it->second.command, "%in", quote_input(call->input));
      if (!rule.placeholder) {
        rule.placeholder = it->second.placeholder;
      }
    }
    if (rule.placeholder) {
      continue;
    }
    Specificity best = Specificity::none;
    for (const Selector& selector : selectors) {
      if (!selector.placeholder) {
        continue;
      }
      if (const Specificity specificity = match(selector, rule.output);
          specificity != Specificity::none && specificity >= best) {
        rule.placeholder = selector.placeholder;
        best = specificity;
      }
    }
  }

  manifest.m_rules.assign(std::make_move_iterator(rules.begin()),
                          std::make_move_iterator(rules.end()));
  return manifest;
}

}  // namespace makebelieve
