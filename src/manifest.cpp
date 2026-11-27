// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace makebelieve {

namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// The leading whitespace-delimited word of @a text, which is already trimmed.
std::string_view first_word(std::string_view text) {
  std::size_t end = 0;
  while (end < text.size() && !is_space(text[end])) {
    ++end;
  }
  return text.substr(0, end);
}

// The action @a word names, or nullopt when it names none.
std::optional<Manifest::Action> to_action(std::string_view word) {
  if (word == "run") {
    return Manifest::Action::Run;
  }
  if (word == "capture") {
    return Manifest::Action::Capture;
  }
  if (word == "copy") {
    return Manifest::Action::Copy;
  }
  if (word == "tracing") {
    return Manifest::Action::Tracing;
  }
  if (word == "status") {
    return Manifest::Action::Status;
  }
  return std::nullopt;
}

// What @a action takes after its name, for the messages that reject a line.
std::string_view argument_of(Manifest::Action action) {
  return action == Manifest::Action::Copy ? "path" : "command";
}

// Normalises @a relative, or returns nullopt when it does not name a file
// inside the directory it is relative to: empty, absolute, escaping with `..`,
// or ending in a separator or `.`. Used for both an output, which must stay
// inside `@/`, and a `copy` source, which must stay inside the manifest's
// directory.
std::optional<std::filesystem::path> to_inside_path(std::string_view relative) {
  std::filesystem::path path =
      std::filesystem::path(relative).lexically_normal();
  if (path.empty() || path.has_root_path() || !path.has_filename() ||
      path.filename() == "." || path.filename() == ".." ||
      *path.begin() == "..") {
    return std::nullopt;
  }
  return path;
}

// Whether @a c can start the name of a variable or a rule.
bool is_name_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_name_char(char c) {
  return is_name_start(c) || (c >= '0' && c <= '9');
}

// Whether @a text names a variable or a rule: a letter or `_`, then letters,
// digits and `_`.
bool is_name(std::string_view text) {
  return !text.empty() && is_name_start(text.front()) &&
         std::ranges::all_of(text, is_name_char);
}

constexpr std::string_view k_in_placeholder = "%in";

// A `$$`, `$name` or `${name}` in some text, from @a begin to @a end. The name
// is empty for `$$`.
struct Reference {
  std::size_t begin;
  std::size_t end;
  std::string_view name;
};

// The reference starting at @a at of @a text, which holds a `$` there, or
// nullopt when what follows the `$` is left for the shell.
std::optional<Reference> reference_at(std::string_view text, std::size_t at) {
  if (at + 1 >= text.size()) {
    return std::nullopt;
  }
  const char next = text[at + 1];
  if (next == '$') {
    return Reference{.begin = at, .end = at + 2, .name = {}};
  }
  if (is_name_start(next)) {
    std::size_t end = at + 2;
    while (end < text.size() && is_name_char(text[end])) {
      ++end;
    }
    return Reference{
        .begin = at, .end = end, .name = text.substr(at + 1, end - at - 1)};
  }
  if (next == '{') {
    const std::size_t close = text.find('}', at + 2);
    if (close != std::string_view::npos &&
        is_name(text.substr(at + 2, close - at - 2))) {
      return Reference{.begin = at,
                       .end = close + 1,
                       .name = text.substr(at + 2, close - at - 2)};
    }
  }
  return std::nullopt;
}

// Stands in an expanded line for something decided only once its captures
// are known: a `$$`, or a reference to a variable that nothing set.
constexpr char k_marker = '\x01';

// Stand in for the captures of a path while it is normalised, around the
// capture's index.
constexpr char k_capture_open = '\x02';
constexpr char k_capture_close = '\x03';

// What a marker stands for.
struct Marker {
  enum class Kind : std::uint8_t {
    // A reference on the output's own line to a variable it does not set.
    OwnReference,
    // A `$$` on the output's own line.
    OwnDollars,
    // A reference in a rule's body to a variable the output does not set.
    BodyReference,
  };
  Kind kind;
  std::string name;
  // As written, for whatever is left as it is.
  std::string written;
};

// A line, or part of one, with its variables expanded as far as they can be
// before its captures are known: each marker in @link text stands for the
// next of @link markers.
struct Expanded {
  std::string text;
  std::vector<Marker> markers;

  void mark(Marker marker) {
    text += k_marker;
    markers.push_back(std::move(marker));
  }
};

// A capture, `*{name}`, `**{name}`, `*{name:regex}` or `**{name:regex}`, from
// @a begin to @a end of the text it is in.
struct Capture {
  std::size_t begin;
  std::size_t end;
  std::string name;
  // Whether it is a `**`, which may hold a `/`.
  bool crosses_directories;
  // The expression it must match, if it was given one.
  std::optional<std::string> constraint;
};

// The capture starting at @a at of @a text, which holds a `*` there, nullopt
// when that `*` is the shell's, or why it is malformed.
std::expected<std::optional<Capture>, std::string> capture_at(
    std::string_view text,
    std::size_t at) {
  std::size_t open = at + 1;
  const bool crosses = open < text.size() && text[open] == '*';
  if (crosses) {
    ++open;
  }
  if (open >= text.size() || text[open] != '{' || open + 1 >= text.size() ||
      !is_name_start(text[open + 1])) {
    return std::nullopt;
  }
  std::size_t end = open + 2;
  while (end < text.size() && is_name_char(text[end])) {
    ++end;
  }
  std::string name(text.substr(open + 1, end - open - 1));
  if (end < text.size() && text[end] == '}') {
    return Capture{.begin = at,
                   .end = end + 1,
                   .name = std::move(name),
                   .crosses_directories = crosses,
                   .constraint = std::nullopt};
  }
  if (end >= text.size() || text[end] != ':') {
    return std::nullopt;
  }
  // The expression runs to the `}` that balances the capture's `{`.
  std::size_t depth = 0;
  std::size_t close = end + 1;
  for (; close < text.size(); ++close) {
    if (text[close] == '\\') {
      ++close;
    } else if (text[close] == '{') {
      ++depth;
    } else if (text[close] == '}') {
      if (depth == 0) {
        break;
      }
      --depth;
    }
  }
  if (close >= text.size()) {
    return std::unexpected(std::format("`{}{}` has no `}}` to end it",
                                       text.substr(at, open - at + 1), name));
  }
  return Capture{
      .begin = at,
      .end = close + 1,
      .name = std::move(name),
      .crosses_directories = crosses,
      .constraint = std::string(text.substr(end + 1, close - end - 1))};
}

// Every capture in @a text, in order, or why one is malformed.
std::expected<std::vector<Capture>, std::string> find_captures(
    std::string_view text) {
  std::vector<Capture> captures;
  for (std::size_t at = 0; at < text.size();) {
    if (text[at] != '*') {
      ++at;
      continue;
    }
    auto found = capture_at(text, at);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    std::optional<Capture>& capture = *found;
    if (!capture.has_value()) {
      ++at;
      continue;
    }
    at = capture->end;
    captures.push_back(std::move(*capture));
  }
  return captures;
}

// The bounds of the whitespace-delimited word of @a text holding @a capture,
// which may itself hold whitespace in its expression.
std::pair<std::size_t, std::size_t> word_around(
    std::string_view text,
    std::span<const Capture> captures,
    const Capture& capture) {
  std::size_t begin = capture.begin;
  while (begin > 0 && !is_space(text[begin - 1])) {
    --begin;
  }
  std::size_t end = begin;
  std::size_t next = 0;
  while (end < text.size()) {
    while (next < captures.size() && captures[next].end <= end) {
      ++next;
    }
    if (next < captures.size() && captures[next].begin <= end) {
      end = captures[next].end;
      continue;
    }
    if (is_space(text[end])) {
      break;
    }
    ++end;
  }
  return {begin, end};
}

// Appends @a text to the end of @a pieces.
void add_literal(Manifest::Template& pieces, std::string_view text) {
  pieces.literals.back() += text;
}

// Appends the value of capture @a index to the end of @a pieces.
void add_capture(Manifest::Template& pieces, std::size_t index) {
  pieces.captures.push_back(index);
  pieces.literals.emplace_back();
}

Manifest::Template empty_template() {
  return Manifest::Template{.literals = {std::string()}, .captures = {}};
}

// @a pieces as one string, each capture as its index between k_capture_open
// and k_capture_close, so it can be normalised as a path.
std::string with_placeholders(const Manifest::Template& pieces) {
  std::string text = pieces.literals.front();
  for (std::size_t i = 0; i < pieces.captures.size(); ++i) {
    text += k_capture_open;
    text += std::to_string(pieces.captures[i]);
    text += k_capture_close;
    text += pieces.literals[i + 1];
  }
  return text;
}

// The inverse of with_placeholders(), or nullopt unless @a text holds the
// captures of @a original in the same order - which normalising can lose, by
// taking `..` past one.
std::optional<Manifest::Template> from_placeholders(
    std::string_view text,
    const Manifest::Template& original) {
  Manifest::Template pieces = empty_template();
  for (std::size_t at = 0; at < text.size();) {
    if (text[at] != k_capture_open) {
      pieces.literals.back() += text[at++];
      continue;
    }
    const std::size_t close = text.find(k_capture_close, at);
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    add_capture(pieces,
                std::stoul(std::string(text.substr(at + 1, close - at - 1))));
    at = close + 1;
  }
  if (pieces.captures != original.captures) {
    return std::nullopt;
  }
  return pieces;
}

// @a pieces normalised as a path inside the directory it is relative to, as
// to_inside_path() does, or nullopt when it is not one.
std::optional<Manifest::Template> to_inside_template(
    const Manifest::Template& pieces) {
  const std::optional<std::filesystem::path> path =
      to_inside_path(with_placeholders(pieces));
  if (!path.has_value()) {
    return std::nullopt;
  }
  return from_placeholders(path->generic_string(), pieces);
}

// @a text with every character an ECMAScript regular expression treats
// specially escaped, so it matches only itself.
std::string escape_regex(std::string_view text) {
  std::string escaped;
  for (const char c : text) {
    if (std::string_view(R"(\^$.|?*+()[]{}/)").contains(c)) {
      escaped += '\\';
    }
    escaped += c;
  }
  return escaped;
}

// The ECMAScript expression @a constraint, for a capture whose group is
// @a group of the expression it goes into: each back reference made to count
// from that group, and - when @a confined - each thing that matches a single
// character kept from matching a `/`, so the capture never holds one whatever
// the expression allows.
std::string place_constraint(std::string_view constraint,
                             std::size_t group,
                             bool confined) {
  std::string placed;
  const auto atom = [&](std::string_view text) {
    if (confined) {
      placed += "(?:(?!/)";
      placed += text;
      placed += ')';
    } else {
      placed += text;
    }
  };
  for (std::size_t at = 0; at < constraint.size();) {
    const char c = constraint[at];
    if (c == '\\' && at + 1 < constraint.size()) {
      const char next = constraint[at + 1];
      if (next >= '1' && next <= '9') {
        std::size_t end = at + 1;
        while (end < constraint.size() && constraint[end] >= '0' &&
               constraint[end] <= '9') {
          ++end;
        }
        placed += '\\';
        placed += std::to_string(
            std::stoul(std::string(constraint.substr(at + 1, end - at - 1))) +
            group);
        at = end;
        continue;
      }
      std::size_t length = 2;
      if (next == 'c') {
        length = 3;
      } else if (next == 'x') {
        length = 4;
      } else if (next == 'u') {
        length = 6;
      }
      length = std::min(length, constraint.size() - at);
      const std::string_view escape = constraint.substr(at, length);
      // Assertions match no character, and these classes never match `/`.
      if (std::string_view("bBdsw").contains(next)) {
        placed += escape;
      } else {
        atom(escape);
      }
      at += length;
      continue;
    }
    if (c == '[') {
      std::size_t end = at + 1;
      if (end < constraint.size() && constraint[end] == '^') {
        ++end;
      }
      while (end < constraint.size() && constraint[end] != ']') {
        end += constraint[end] == '\\' ? 2 : 1;
      }
      end = std::min(end + 1, constraint.size());
      atom(constraint.substr(at, end - at));
      at = end;
      continue;
    }
    if (c == '.' || c == '/') {
      atom(constraint.substr(at, 1));
    } else {
      placed += c;
    }
    ++at;
  }
  return placed;
}

// One step of a pattern, as far as which paths it could match goes: a
// character, or a capture - one or more characters, `/` among them only if
// it may cross directories. A capture's own expression is not looked at, so
// this may say two patterns overlap when they do not, but never the reverse.
struct GlobStep {
  char literal = '\0';
  bool any = false;
  bool crosses_directories = false;
};

// The steps of @a pieces, whose capture `i` crosses directories if
// @a crosses_directories says so.
std::vector<GlobStep> glob_steps(const Manifest::Template& pieces,
                                 const std::vector<bool>& crosses_directories) {
  std::vector<GlobStep> steps;
  for (std::size_t i = 0; i < pieces.literals.size(); ++i) {
    if (i > 0) {
      steps.push_back(
          {.literal = '\0',
           .any = true,
           .crosses_directories = crosses_directories[pieces.captures[i - 1]]});
    }
    for (const char c : pieces.literals[i]) {
      steps.push_back({.literal = c});
    }
  }
  return steps;
}

// Whether some path matches both @a a and @a b. Each is run as a machine
// whose state `2k` is about to match step `k` and `2k + 1` is inside capture
// `k` having matched at least one character, and the two are run together
// over every character that could tell them apart.
bool overlaps(const std::vector<GlobStep>& a, const std::vector<GlobStep>& b) {
  std::set<char> alphabet = {'/', '\0'};
  for (const std::vector<GlobStep>* steps : {&a, &b}) {
    for (const GlobStep& step : *steps) {
      if (!step.any) {
        alphabet.insert(step.literal);
      }
    }
  }
  const auto next = [](const std::vector<GlobStep>& steps, std::size_t state,
                       char c) {
    std::vector<std::size_t> states;
    const std::size_t k = state / 2;
    if (k >= steps.size()) {
      return states;
    }
    const GlobStep& step = steps[k];
    if (!step.any) {
      if (state % 2 == 0 && step.literal == c && c != '\0') {
        states.push_back(state + 2);
      }
      return states;
    }
    if (c != '/' || step.crosses_directories) {
      // Having matched a character it may stop, or go on.
      states.push_back((2 * k) + 1);
      states.push_back((2 * k) + 2);
    }
    return states;
  };
  std::set<std::pair<std::size_t, std::size_t>> seen = {{0, 0}};
  std::vector<std::pair<std::size_t, std::size_t>> pending = {{0, 0}};
  while (!pending.empty()) {
    const auto [x, y] = pending.back();
    pending.pop_back();
    if (x == 2 * a.size() && y == 2 * b.size()) {
      return true;
    }
    for (const char c : alphabet) {
      for (const std::size_t x2 : next(a, x, c)) {
        for (const std::size_t y2 : next(b, y, c)) {
          if (seen.emplace(x2, y2).second) {
            pending.emplace_back(x2, y2);
          }
        }
      }
    }
  }
  return false;
}
// A line of the manifest that is neither blank nor a comment, trimmed.
struct Line {
  std::size_t number;
  std::string_view text;
  // Whether it began with whitespace, which makes it a variable of the line
  // above it.
  bool indented;
};

// A variable set by an indented `name = value` line.
struct Binding {
  std::string value;
  std::size_t line;
};
using Bindings = std::map<std::string, Binding, std::less<>>;

// A `!name = <action> <body>` rule, which outputs declared below it use.
struct Declared {
  Manifest::Action action;
  std::string body;
  // The variables its body names, and the values of those given a default.
  std::set<std::string, std::less<>> variables;
  Bindings defaults;
  // Whether its body holds a `%in` for a use's argument.
  bool takes_input;
  std::size_t line;
};

// What a marker turns out to be once the captures are known: text to put in,
// or the capture whose value goes there.
using Resolved = std::variant<std::string, std::size_t>;

using Names = std::set<std::string, std::less<>>;

// Appends @a text, a part of an output's own line, to @a into: each variable
// @a given sets replaced by its value, and every other reference and `$$`
// marked, to be decided once the captures are known. Each name referred to is
// added to @a named.
void expand_own(Expanded& into,
                std::string_view text,
                const Bindings& given,
                Names& named) {
  for (std::size_t at = 0; at < text.size();) {
    const std::optional<Reference> reference =
        text[at] == '$' ? reference_at(text, at) : std::nullopt;
    if (!reference.has_value()) {
      into.text += text[at++];
      continue;
    }
    const std::string_view written =
        text.substr(reference->begin, reference->end - reference->begin);
    if (reference->name.empty()) {
      into.mark({.kind = Marker::Kind::OwnDollars,
                 .name = {},
                 .written = std::string(written)});
    } else {
      named.emplace(reference->name);
      if (const auto set = given.find(reference->name); set != given.end()) {
        into.text += set->second.value;
      } else {
        into.mark({.kind = Marker::Kind::OwnReference,
                   .name = std::string(reference->name),
                   .written = std::string(written)});
      }
    }
    at = reference->end;
  }
}

// Appends the body of @a rule to @a into, as used with @a input for its `%in`
// - expanded as expand_own() does - and each of its variables replaced by
// the value @a given sets or, unless it is one of @a deferred, by its
// default; any other is marked, to be decided once the captures are known.
// `$$` is a `$`.
void expand_body(Expanded& into,
                 const Declared& rule,
                 std::string_view input,
                 const Bindings& given,
                 const Names& deferred,
                 Names& named) {
  const std::string_view body = rule.body;
  for (std::size_t at = 0; at < body.size();) {
    if (body.substr(at).starts_with(k_in_placeholder)) {
      expand_own(into, input, given, named);
      at += k_in_placeholder.size();
      continue;
    }
    const std::optional<Reference> reference =
        body[at] == '$' ? reference_at(body, at) : std::nullopt;
    if (!reference.has_value()) {
      into.text += body[at++];
      continue;
    }
    if (reference->name.empty()) {
      into.text += '$';
    } else if (const auto set = given.find(reference->name);
               set != given.end()) {
      into.text += set->second.value;
    } else if (const auto fallback = rule.defaults.find(reference->name);
               fallback != rule.defaults.end() &&
               !deferred.contains(reference->name)) {
      into.text += fallback->second.value;
    } else {
      into.mark({.kind = Marker::Kind::BodyReference,
                 .name = std::string(reference->name),
                 .written = {}});
    }
    at = reference->end;
  }
}

}  // namespace

std::string Manifest::Template::fill(
    std::span<const std::string> values) const {
  std::string text = literals.front();
  for (std::size_t i = 0; i < captures.size(); ++i) {
    text += values[captures[i]];
    text += literals[i + 1];
  }
  return text;
}

std::string Manifest::Template::spelled(
    std::span<const std::string> names) const {
  std::string text = literals.front();
  for (std::size_t i = 0; i < captures.size(); ++i) {
    text += "${";
    text += names[captures[i]];
    text += '}';
    text += literals[i + 1];
  }
  return text;
}

std::optional<Manifest::Rule> Manifest::instantiate(const Rule& rule,
                                                    std::string_view path) {
  const Pattern& pattern = *rule.pattern;
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_match(path.begin(), path.end(), match, pattern.regex)) {
    return std::nullopt;
  }
  std::vector<std::string> values;
  values.reserve(pattern.groups.size());
  for (const std::size_t group : pattern.groups) {
    values.push_back(match[static_cast<int>(group)].str());
  }
  std::optional<std::filesystem::path> output =
      to_inside_path(pattern.output.fill(values));
  if (!output.has_value()) {
    return std::nullopt;
  }
  return Rule{.output = std::move(*output),
              .action = rule.action,
              .command = pattern.command.fill(values),
              .text = rule.text,
              .pattern = nullptr,
              .line = rule.line};
}
Manifest Manifest::parse(std::string_view text) {
  Manifest manifest;

  // Where each accepted output, and each directory above one, was declared, so
  // a later rule cannot reuse a path as both a file and a directory.
  std::map<std::filesystem::path, std::size_t> files;
  std::map<std::filesystem::path, std::size_t> directories;

  // Accepted copies of outputs, checked once every output is known.
  struct CopyOfOutput {
    std::size_t rule;
    std::size_t line;
    std::string left;
    std::filesystem::path source;
  };
  std::vector<CopyOfOutput> copies;

  // The rules declared so far, by name without the `!`.
  std::map<std::string, Declared, std::less<>> declared;

  std::vector<Line> lines;
  std::size_t start = 0;
  for (std::size_t number = 1; start <= text.size(); ++number) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end =
        newline == std::string_view::npos ? text.size() : newline;
    const std::string_view raw = text.substr(start, end - start);
    const std::string_view line = trim(raw);
    start = end + 1;
    if (!line.empty() && line.front() != '#') {
      lines.push_back({.number = number,
                       .text = line,
                       .indented = raw.front() == ' ' || raw.front() == '\t'});
    }
  }

  for (std::size_t next = 0; next < lines.size();) {
    const std::size_t number = lines[next].number;
    const std::string_view line = lines[next].text;
    const bool indented = lines[next].indented;
    ++next;

    const auto reject_at = [&](std::size_t at, std::string message) {
      manifest.m_errors.push_back({.line = at, .message = std::move(message)});
    };
    const auto reject = [&](std::string message) {
      reject_at(number, std::move(message));
    };

    if (indented) {
      reject(
          "an indented line sets a variable of the rule above it, but "
          "there is none");
      continue;
    }

    // The indented lines under this one, which set its variables. They are
    // passed over without comment when the line itself is rejected.
    const std::size_t first_under = next;
    while (next < lines.size() && lines[next].indented) {
      ++next;
    }
    const std::span<const Line> under =
        std::span<const Line>(lines).subspan(first_under, next - first_under);

    // The variables @a under sets, or nullopt, having rejected the lines at
    // fault, when any is not a `name = value` or sets a variable again.
    const auto bindings_of = [&]() -> std::optional<Bindings> {
      Bindings bindings;
      bool accepted = true;
      for (const Line& binding : under) {
        const std::size_t equals = binding.text.find('=');
        const std::string_view name = trim(binding.text.substr(0, equals));
        if (equals == std::string_view::npos || !is_name(name)) {
          reject_at(binding.number, "expected `<variable> = <value>`");
          accepted = false;
        } else if (!bindings
                        .emplace(name,
                                 Binding{.value = std::string(trim(
                                             binding.text.substr(equals + 1))),
                                         .line = binding.number})
                        .second) {
          reject_at(binding.number, std::format("`{}` is already set", name));
          accepted = false;
        }
      }
      return accepted ? std::optional(std::move(bindings)) : std::nullopt;
    };

    // Rejects each variable of @a bindings that the rule named @a name does
    // not use, returning whether there were none.
    const auto all_used = [&](const Bindings& bindings, const Declared& rule,
                              std::string_view name) {
      bool used = true;
      for (const auto& [variable, binding] : bindings) {
        if (!rule.variables.contains(variable)) {
          reject_at(binding.line, std::format("rule `!{}` does not use `${}`",
                                              name, variable));
          used = false;
        }
      }
      return used;
    };

    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      reject("expected `@/<output> = <action> <command>`");
      continue;
    }

    const std::string_view left = trim(line.substr(0, equals));
    const std::string_view value = trim(line.substr(equals + 1));
    const std::string_view word = first_word(value);

    // `!name = <action> <body>` declares a rule for the outputs below it to
    // use; the indented lines under it give its variables default values.
    if (left.starts_with('!')) {
      const std::string_view name = left.substr(1);
      if (!is_name(name)) {
        reject(
            std::format("rule `{}` must be named with letters, digits and "
                        "`_`, not starting with a digit",
                        left));
        continue;
      }
      const std::optional<Manifest::Action> action = to_action(word);
      if (!action.has_value() || Manifest::is_report(*action)) {
        reject(
            std::format("rule `{}` must be assigned `run <command>`, "
                        "`capture <command>` or `copy <path>`",
                        left));
        continue;
      }
      const std::string_view body = trim(value.substr(word.size()));
      if (body.empty()) {
        reject(std::format("rule `{}` has no {}", left, argument_of(*action)));
        continue;
      }
      if (const auto it = declared.find(name); it != declared.end()) {
        reject(std::format("rule `{}` is already declared on line {}", left,
                           it->second.line));
        continue;
      }
      std::optional<Bindings> defaults = bindings_of();
      if (!defaults.has_value()) {
        continue;
      }
      Declared rule{.action = *action,
                    .body = std::string(body),
                    .variables = {},
                    .defaults = std::move(*defaults),
                    .takes_input = body.contains(k_in_placeholder),
                    .line = number};
      for (std::size_t at = 0; at < body.size();) {
        const std::optional<Reference> reference =
            body[at] == '$' ? reference_at(body, at) : std::nullopt;
        if (!reference.has_value()) {
          ++at;
          continue;
        }
        if (!reference->name.empty()) {
          rule.variables.emplace(reference->name);
        }
        at = reference->end;
      }
      if (all_used(rule.defaults, rule, name)) {
        declared.emplace(name, std::move(rule));
      }
      continue;
    }

    if (!left.starts_with("@/")) {
      reject(std::format("output `{}` must start with `@/`", left));
      continue;
    }
    if (line.find_first_of("\x01\x02\x03") != std::string_view::npos) {
      reject(std::format("`{}` holds a control character", left));
      continue;
    }
    if (left.contains('*')) {
      reject(std::format(
          "`{}` has a `*` in its path, which captures nothing: capture part "
          "of its source pattern with `*{{name}}` and use it in the path as "
          "`$name`",
          left));
      continue;
    }

    // An output is assigned an action and what that action applies to; the
    // rest of the line after the action is that argument. Assigned a `!rule`
    // instead, it takes the rule's action, and for its argument the rule's
    // body with `%in` replaced by the rest of the line and each variable by
    // its value: the one set under this line, or captured, or else the rule's
    // default.
    const Declared* rule = nullptr;
    std::optional<Manifest::Action> action;
    std::string_view input;
    std::string_view argument;
    if (word.starts_with('!')) {
      const auto it = declared.find(word.substr(1));
      if (it == declared.end()) {
        reject(std::format("`{}` uses `{}`, which no rule above it declares",
                           left, word));
        continue;
      }
      rule = &it->second;
      input = trim(value.substr(word.size()));
      if (rule->takes_input && input.empty()) {
        reject(
            std::format("`{}` gives `{}` nothing for its `%in`", left, word));
        continue;
      }
      if (!rule->takes_input && !input.empty()) {
        reject(
            std::format("`{}` gives `{}` `{}`, but it has no `%in` to "
                        "take it",
                        left, word, input));
        continue;
      }
      action = rule->action;
    } else {
      action = to_action(word);
      argument = trim(value.substr(word.size()));
    }
    if (!action.has_value()) {
      reject(
          std::format("`{}` must be assigned `run <command>`, "
                      "`capture <command>`, `copy <path>`, `tracing`, "
                      "`status` or a `!<rule>`",
                      left));
      continue;
    }
    const std::optional<Bindings> given = bindings_of();
    if (!given.has_value()) {
      continue;
    }

    // Expanded as far as it can be before the captures are known, which are
    // found in the argument once any rule is expanded. A capture overriding
    // a rule's default needs that default left out, so it is expanded again.
    Names named;
    Expanded path;
    expand_own(path, left.substr(2), *given, named);
    const auto expand_argument = [&](const Names& deferred) {
      Expanded expanded;
      if (rule != nullptr) {
        expand_body(expanded, *rule, input, *given, deferred, named);
      } else {
        expand_own(expanded, argument, *given, named);
      }
      return expanded;
    };
    Expanded expanded = expand_argument({});
    auto found = find_captures(expanded.text);
    if (found.has_value() && rule != nullptr) {
      Names deferred;
      for (const Capture& capture : *found) {
        if (rule->defaults.contains(capture.name)) {
          deferred.insert(capture.name);
        }
      }
      if (!deferred.empty()) {
        expanded = expand_argument(deferred);
        found = find_captures(expanded.text);
      }
    }
    if (!found.has_value()) {
      reject(std::format("`{}`: {}", left, found.error()));
      continue;
    }
    const std::vector<Capture>& captures = *found;

    bool all_named = true;
    for (const auto& [variable, binding] : *given) {
      if ((rule == nullptr || !rule->variables.contains(variable)) &&
          !named.contains(variable)) {
        reject_at(binding.line,
                  rule != nullptr
                      ? std::format("neither `{}` nor the line of `{}` uses "
                                    "`${}`",
                                    word, left, variable)
                      : std::format("`{}` sets `${}`, which its line never "
                                    "uses",
                                    left, variable));
        all_named = false;
      }
    }
    if (!all_named) {
      continue;
    }

    const std::string_view trimmed = trim(expanded.text);
    if (Manifest::is_report(*action)) {
      if (!trimmed.empty()) {
        reject(
            std::format("`{}` is assigned `{}`, which takes no "
                        "argument, but was given `{}`",
                        left, word, argument));
        continue;
      }
    } else if (trimmed.empty()) {
      reject(std::format("`{}` has no {}", left, argument_of(*action)));
      continue;
    }

    // The source pattern: the first word to hold a capture, or a copy's whole
    // path. Every capture must be in it, each name captured once and not set.
    std::map<std::string, std::size_t, std::less<>> index_of;
    std::size_t source_begin = 0;
    std::size_t source_end = 0;
    if (!captures.empty()) {
      if (*action == Manifest::Action::Copy) {
        source_begin = expanded.text.find_first_not_of(" \t\r\n");
        source_end = expanded.text.find_last_not_of(" \t\r\n") + 1;
      } else {
        std::tie(source_begin, source_end) =
            word_around(expanded.text, captures, captures.front());
      }
      std::string problem;
      for (const Capture& capture : captures) {
        const std::string_view written =
            std::string_view(expanded.text)
                .substr(capture.begin, capture.end - capture.begin);
        if (written.contains(k_marker)) {
          problem = std::format("`{}` uses a variable inside a capture", left);
        } else if (capture.begin < source_begin || capture.end > source_end) {
          problem = std::format(
              "`{}` has `{}` outside its source pattern, the first word to "
              "hold a capture, which is the only place captures go",
              left, written);
        } else if (given->contains(capture.name)) {
          problem = std::format("`{}` both captures and sets `${}`", left,
                                capture.name);
        } else if (!index_of.emplace(capture.name, index_of.size()).second) {
          problem =
              std::format("`{}` captures `${}` twice", left, capture.name);
        }
        if (!problem.empty()) {
          break;
        }
      }
      if (!problem.empty()) {
        reject(std::move(problem));
        continue;
      }
    }
    const bool expands_anything = !given->empty() || !captures.empty();

    // What each marker turns out to be, or nullopt having said why in
    // @a problem. In the path, a variable must be captured or set.
    std::string problem;
    const auto resolve = [&](const Marker& marker,
                             bool in_path) -> std::optional<Resolved> {
      const auto captured = index_of.find(marker.name);
      switch (marker.kind) {
        case Marker::Kind::OwnDollars:
          return Resolved(std::string(expands_anything ? "$" : "$$"));
        case Marker::Kind::OwnReference:
          if (captured != index_of.end()) {
            return Resolved(captured->second);
          }
          if (rule != nullptr && rule->defaults.contains(marker.name)) {
            problem = std::format(
                "`{}` names `${}` without setting it, but the default `{}` "
                "gives it is for the rule's body alone",
                left, marker.name, word);
            return std::nullopt;
          }
          if (in_path) {
            problem = std::format(
                "`{}` uses `${}` in its path, which it neither captures nor "
                "sets",
                left, marker.name);
            return std::nullopt;
          }
          return Resolved(marker.written);
        case Marker::Kind::BodyReference:
          if (captured != index_of.end()) {
            return Resolved(captured->second);
          }
          if (const auto fallback = rule->defaults.find(marker.name);
              fallback != rule->defaults.end()) {
            return Resolved(fallback->second.value);
          }
          problem = std::format(
              "`{}` uses `{}` without setting `${}`, which has no default",
              left, word, marker.name);
          return std::nullopt;
      }
      return std::nullopt;
    };

    // The text from @a begin to @a end of @a from with its markers resolved
    // and the captures of @a as_captures, which lie within it, as theirs -
    // setting @a by_name if a marker turned out to be a capture.
    const auto render =
        [&](const Expanded& from, std::size_t begin, std::size_t end,
            std::span<const Capture> as_captures, bool in_path,
            bool& by_name) -> std::optional<Manifest::Template> {
      Manifest::Template pieces = empty_template();
      auto marker = static_cast<std::size_t>(std::count(
          from.text.begin(),
          from.text.begin() + static_cast<std::ptrdiff_t>(begin), k_marker));
      std::size_t next = 0;
      for (std::size_t at = begin; at < end;) {
        if (next < as_captures.size() && as_captures[next].begin == at) {
          add_capture(pieces, index_of.at(as_captures[next].name));
          at = as_captures[next++].end;
          continue;
        }
        if (from.text[at] != k_marker) {
          pieces.literals.back() += from.text[at++];
          continue;
        }
        const std::optional<Resolved> resolved =
            resolve(from.markers[marker++], in_path);
        if (!resolved.has_value()) {
          return std::nullopt;
        }
        if (const auto* literal = std::get_if<std::string>(&*resolved)) {
          add_literal(pieces, *literal);
        } else {
          add_capture(pieces, std::get<std::size_t>(*resolved));
          by_name = true;
        }
        ++at;
      }
      return pieces;
    };

    bool unused = false;
    const std::optional<Manifest::Template> output_pieces =
        render(path, 0, path.text.size(), {}, true, unused);
    if (!output_pieces.has_value()) {
      reject(std::move(problem));
      continue;
    }
    const std::optional<Manifest::Template> command_pieces =
        render(expanded, 0, expanded.text.size(), captures, false, unused);
    if (!command_pieces.has_value()) {
      reject(std::move(problem));
      continue;
    }

    std::string command;
    std::optional<std::filesystem::path> output;
    std::optional<std::filesystem::path> copied_output;
    std::shared_ptr<Manifest::Pattern> pattern;
    // The directory the output is in, as far as it is known here.
    std::filesystem::path output_directory;

    if (captures.empty()) {
      // A `run` or `capture` command is taken verbatim. `copy` names a file
      // instead, held to the same shape as an output - inside the directory,
      // no escaping - so every copy's source is one the build can watch, and
      // stored normalised. One under `@/` is another output.
      const std::string_view whole = trim(command_pieces->literals.front());
      output = to_inside_path(output_pieces->literals.front());
      command = std::string(whole);
      if (*action == Manifest::Action::Copy) {
        const bool generated = whole.starts_with("@/");
        const std::optional<std::filesystem::path> source =
            to_inside_path(generated ? whole.substr(2) : whole);
        if (!source.has_value()) {
          reject(std::format(
              "`{}` copies `{}`, which does not name a file "
              "inside {}",
              left, whole, generated ? "`@/`" : "the manifest's directory"));
          continue;
        }
        if (generated) {
          if (source == output) {
            reject(std::format("`{}` copies itself", left));
            continue;
          }
          copied_output = source;
          command = "@/" + source->generic_string();
        } else {
          command = source->generic_string();
        }
      }
      if (!output.has_value()) {
        reject(std::format("`{}` does not name a file inside `@/`", left));
        continue;
      }
      output_directory = output->parent_path();
    } else {
      bool by_name = false;
      std::optional<Manifest::Template> source =
          render(expanded, source_begin, source_end, captures, false, by_name);
      const auto spell_source = [&](const Manifest::Template& pieces) {
        std::string text = pieces.literals.front();
        for (std::size_t i = 0; i < pieces.captures.size(); ++i) {
          const Capture& capture = captures[pieces.captures[i]];
          text += std::string_view(expanded.text)
                      .substr(capture.begin, capture.end - capture.begin);
          text += pieces.literals[i + 1];
        }
        return text;
      };
      if (by_name) {
        reject(std::format(
            "`{}` uses a variable it captures in its source pattern `{}`", left,
            spell_source(*source)));
        continue;
      }
      const bool over_outputs = source->literals.front().starts_with("@/");
      if (over_outputs) {
        source->literals.front().erase(0, 2);
      }
      const bool quoted =
          std::ranges::any_of(source->literals, [](const std::string& literal) {
            return literal.find_first_of("\"'") != std::string::npos;
          });
      const std::optional<Manifest::Template> normal =
          quoted ? std::nullopt : to_inside_template(*source);
      if (!normal.has_value()) {
        reject(std::format(
            "`{}` takes its sources from `{}{}`, which must be an unquoted "
            "path inside {}",
            left, over_outputs ? "@/" : "", spell_source(*source),
            over_outputs ? "`@/`" : "the manifest's directory"));
        continue;
      }
      const std::optional<Manifest::Template> output_normal =
          to_inside_template(*output_pieces);
      if (!output_normal.has_value()) {
        reject(std::format("`{}` does not name a file inside `@/`", left));
        continue;
      }
      std::string unused_capture;
      for (const Capture& capture : captures) {
        if (!std::ranges::contains(output_normal->captures,
                                   index_of.at(capture.name))) {
          unused_capture = capture.name;
          break;
        }
      }
      if (!unused_capture.empty()) {
        reject(std::format(
            "`{}` captures `${}` but never uses it in its path, so files that "
            "differ only there would make the same output",
            left, unused_capture));
        continue;
      }

      pattern = std::make_shared<Manifest::Pattern>();
      pattern->source = spell_source(*normal);
      pattern->over_outputs = over_outputs;
      pattern->pieces = *normal;
      pattern->output = *output_normal;
      for (const Capture& capture : captures) {
        pattern->names.push_back(capture.name);
        pattern->crosses_directories.push_back(capture.crosses_directories);
      }
      const std::string& start_text = normal->literals.front();
      const std::size_t slash = start_text.rfind('/');
      pattern->directory =
          slash == std::string::npos
              ? std::filesystem::path()
              : std::filesystem::path(start_text.substr(0, slash))
                    .lexically_normal();
      pattern->recursive =
          std::ranges::any_of(captures, &Capture::crosses_directories) ||
          std::ranges::any_of(
              normal->literals | std::views::drop(1),
              [](const std::string& literal) { return literal.contains('/'); });
      if (*action == Manifest::Action::Copy) {
        pattern->command = *normal;
        if (over_outputs) {
          pattern->command.literals.front().insert(0, "@/");
        }
      } else {
        pattern->command = *command_pieces;
      }

      // One regular expression for the whole path, each capture in a group
      // of its own: shortest first, unless its own expression says otherwise.
      std::string expression = escape_regex(normal->literals.front());
      std::size_t group = 0;
      std::string bad_constraint;
      for (std::size_t i = 0; i < captures.size(); ++i) {
        const Capture& capture = captures[normal->captures[i]];
        pattern->groups.push_back(++group);
        if (!capture.constraint.has_value()) {
          expression +=
              capture.crosses_directories ? R"(([\s\S]+?))" : "([^/]+?)";
        } else {
          try {
            const std::regex alone(*capture.constraint, std::regex::ECMAScript);
            expression += '(';
            expression += place_constraint(*capture.constraint, group,
                                           !capture.crosses_directories);
            expression += ')';
            group += alone.mark_count();
          } catch (const std::regex_error&) {
            bad_constraint = std::string(
                std::string_view(expanded.text)
                    .substr(capture.begin, capture.end - capture.begin));
            break;
          }
        }
        expression += escape_regex(normal->literals[i + 1]);
      }
      if (bad_constraint.empty()) {
        try {
          pattern->regex = std::regex(expression, std::regex::ECMAScript);
        } catch (const std::regex_error&) {
          bad_constraint = pattern->source;
        }
      }
      if (!bad_constraint.empty()) {
        reject(std::format(
            "`{}` has `{}`, whose expression is not a valid ECMAScript "
            "regular expression",
            left, bad_constraint));
        continue;
      }

      output = std::filesystem::path(output_normal->spelled(pattern->names));
      command = pattern->command.spelled(pattern->names);
      const std::string& output_start = output_normal->literals.front();
      const std::size_t output_slash = output_start.rfind('/');
      if (output_slash != std::string::npos) {
        output_directory =
            std::filesystem::path(output_start.substr(0, output_slash))
                .lexically_normal();
      }
    }

    if (pattern == nullptr) {
      if (const auto it = files.find(*output); it != files.end()) {
        reject(std::format("`{}` is already declared on line {}", left,
                           it->second));
        continue;
      }
      if (const auto it = directories.find(*output); it != directories.end()) {
        reject(
            std::format("`{}` is a directory of the output declared on line {}",
                        left, it->second));
        continue;
      }
    }
    std::optional<std::size_t> clash;
    for (std::filesystem::path parent = output_directory; !parent.empty();
         parent = parent.parent_path()) {
      if (const auto it = files.find(parent); it != files.end()) {
        clash = it->second;
        break;
      }
    }
    if (clash.has_value()) {
      reject(std::format("`{}` is inside the output declared on line {}", left,
                         *clash));
      continue;
    }

    if (pattern == nullptr) {
      files.emplace(*output, number);
    }
    for (std::filesystem::path parent = output_directory; !parent.empty();
         parent = parent.parent_path()) {
      directories.emplace(parent, number);
    }
    if (copied_output.has_value()) {
      copies.push_back({.rule = manifest.m_rules.size(),
                        .line = number,
                        .left = std::string(left),
                        .source = std::move(*copied_output)});
    }
    manifest.m_rules.push_back({.output = std::move(*output),
                                .action = *action,
                                .command = std::move(command),
                                .text = std::string(line),
                                .pattern = std::move(pattern),
                                .line = number});
  }

  // The rules that turn out not to stand once every rule is known, by index.
  std::set<std::size_t> dropped;

  // A copy may name an output declared after it.
  for (const CopyOfOutput& copy : copies) {
    if (!files.contains(copy.source)) {
      manifest.m_errors.push_back(
          {.line = copy.line,
           .message = std::format("`{}` copies `@/{}`, which is not declared",
                                  copy.left, copy.source.generic_string())});
      dropped.insert(copy.rule);
    }
  }

  // A pattern over outputs must not be able to feed its own sources, directly
  // or through others, or its outputs would never end: `feeds[a][b]` once
  // something rule `a` could produce is something rule `b` could match, then
  // closed over every chain of such rules.
  const std::span<const Rule> rules = manifest.m_rules;
  std::vector<std::vector<GlobStep>> produces(rules.size());
  std::vector<std::vector<GlobStep>> matches(rules.size());
  for (std::size_t i = 0; i < rules.size(); ++i) {
    if (const Pattern* pattern = rules[i].pattern.get()) {
      produces[i] = glob_steps(pattern->output, pattern->crosses_directories);
      matches[i] = glob_steps(pattern->pieces, pattern->crosses_directories);
    }
  }
  std::vector<std::vector<bool>> feeds(rules.size(),
                                       std::vector<bool>(rules.size()));
  for (std::size_t a = 0; a < rules.size(); ++a) {
    for (std::size_t b = 0; b < rules.size(); ++b) {
      feeds[a][b] = rules[a].is_pattern() && rules[b].is_pattern() &&
                    rules[b].pattern->over_outputs &&
                    overlaps(produces[a], matches[b]);
    }
  }
  for (std::size_t via = 0; via < rules.size(); ++via) {
    for (std::size_t a = 0; a < rules.size(); ++a) {
      for (std::size_t b = 0; b < rules.size(); ++b) {
        feeds[a][b] = feeds[a][b] || (feeds[a][via] && feeds[via][b]);
      }
    }
  }
  for (std::size_t rule = 0; rule < rules.size(); ++rule) {
    if (feeds[rule][rule]) {
      manifest.m_errors.push_back(
          {.line = rules[rule].line,
           .message = std::format(
               "`@/{}` could produce its own sources, so its outputs would "
               "never end",
               rules[rule].output.generic_string())});
      dropped.insert(rule);
    }
  }

  // Dropped last first, so earlier indices still hold.
  for (const std::size_t rule : std::views::reverse(dropped)) {
    manifest.m_rules.erase(manifest.m_rules.begin() +
                           static_cast<std::ptrdiff_t>(rule));
  }
  std::ranges::stable_sort(manifest.m_errors, {}, &Manifest::Error::line);

  return manifest;
}

}  // namespace makebelieve
