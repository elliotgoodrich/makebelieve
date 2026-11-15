// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

// The first whitespace-delimited word of @a text to hold a `*`, or an empty
// view when none does.
std::string_view first_wildcard_word(std::string_view text) {
  for (text = trim(text); !text.empty(); text = trim(text)) {
    const std::string_view word = first_word(text);
    if (word.contains('*')) {
      return word;
    }
    text.remove_prefix(word.size());
  }
  return {};
}

// Whether @a path has exactly one `*`, in its file name.
bool is_wildcard_pattern(const std::filesystem::path& path) {
  const std::string text = path.generic_string();
  return std::ranges::count(text, '*') == 1 &&
         path.filename().generic_string().contains('*');
}

// A file name with a single `*`, as what surrounds it. The `*` matches one or
// more characters.
struct NamePattern {
  std::string prefix;
  std::string suffix;

  // @pre @a name holds a `*`.
  explicit NamePattern(std::string_view name)
      : prefix(name.substr(0, name.find('*'))),
        suffix(name.substr(name.find('*') + 1)) {}

  [[nodiscard]] bool matches(std::string_view name) const {
    return name.size() > prefix.size() + suffix.size() &&
           name.starts_with(prefix) && name.ends_with(suffix);
  }

  // Whether some name matches both this and @a other: the `*`s can stretch to
  // cover any difference, so only the fixed ends have to agree.
  [[nodiscard]] bool overlaps(const NamePattern& other) const {
    const auto agree = [](std::string_view a, std::string_view b, bool front) {
      const std::string_view longer = a.size() >= b.size() ? a : b;
      const std::string_view shorter = a.size() >= b.size() ? b : a;
      return front ? longer.starts_with(shorter) : longer.ends_with(shorter);
    };
    return agree(prefix, other.prefix, true) &&
           agree(suffix, other.suffix, false);
  }
};

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

// The body of a rule with every `$name` replaced by what @a value_of returns
// for `name`, and every `%in` by what it returns for `%in`, which no variable
// can be named. `$$` stands for a single `$`, and a `$` before anything else
// that cannot start a name is left for the shell. What is substituted in is
// not looked through again.
template <typename ValueOf>
std::string substitute(std::string_view body, ValueOf&& value_of) {
  std::string result;
  for (std::size_t i = 0; i < body.size();) {
    if (body.substr(i).starts_with(k_in_placeholder)) {
      result += value_of(k_in_placeholder);
      i += k_in_placeholder.size();
      continue;
    }
    if (body[i] == '$' && i + 1 < body.size()) {
      if (body[i + 1] == '$') {
        result += '$';
        i += 2;
        continue;
      }
      if (is_name_start(body[i + 1])) {
        std::size_t end = i + 2;
        while (end < body.size() && is_name_char(body[end])) {
          ++end;
        }
        result += value_of(body.substr(i + 1, end - i - 1));
        i = end;
        continue;
      }
    }
    result += body[i++];
  }
  return result;
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

}  // namespace

std::optional<Manifest::Rule> Manifest::instantiate(
    const Rule& rule,
    std::string_view source_name) {
  const NamePattern source(rule.wildcard_source.filename().generic_string());
  if (!source.matches(source_name)) {
    return std::nullopt;
  }
  const std::string_view stem = source_name.substr(
      source.prefix.size(),
      source_name.size() - source.prefix.size() - source.suffix.size());

  const NamePattern output(rule.output.filename().generic_string());
  std::string command;
  for (const char c : rule.command) {
    if (c == '*') {
      command += stem;
    } else {
      command += c;
    }
  }
  return Rule{.output = rule.output.parent_path() /
                        (output.prefix + std::string(stem) + output.suffix),
              .action = rule.action,
              .command = std::move(command),
              .wildcard_source = {},
              .wildcard_over_outputs = false,
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
      (void)substitute(body, [&](std::string_view variable) {
        if (variable != k_in_placeholder) {
          rule.variables.emplace(variable);
        }
        return std::string_view();
      });
      if (all_used(rule.defaults, rule, name)) {
        declared.emplace(name, std::move(rule));
      }
      continue;
    }

    if (!left.starts_with("@/")) {
      reject(std::format("output `{}` must start with `@/`", left));
      continue;
    }

    // An output is assigned an action and what that action applies to; the
    // rest of the line after the action is that argument. Assigned a `!rule`
    // instead, it takes the rule's action, and for its argument the rule's
    // body with `%in` replaced by the rest of the line and each variable by
    // its value: the one set under this line, or else the rule's default.
    std::optional<Manifest::Action> action;
    std::string expanded;
    std::string_view argument;
    if (word.starts_with('!')) {
      const std::string_view name = word.substr(1);
      const auto it = declared.find(name);
      if (it == declared.end()) {
        reject(std::format("`{}` uses `{}`, which no rule above it declares",
                           left, word));
        continue;
      }
      const Declared& rule = it->second;
      const std::string_view input = trim(value.substr(word.size()));
      if (rule.takes_input && input.empty()) {
        reject(
            std::format("`{}` gives `{}` nothing for its `%in`", left, word));
        continue;
      }
      if (!rule.takes_input && !input.empty()) {
        reject(
            std::format("`{}` gives `{}` `{}`, but it has no `%in` to "
                        "take it",
                        left, word, input));
        continue;
      }
      const std::optional<Bindings> given = bindings_of();
      if (!given.has_value() || !all_used(*given, rule, name)) {
        continue;
      }
      std::string_view unset;
      expanded = substitute(rule.body, [&](std::string_view variable) {
        if (variable == k_in_placeholder) {
          return input;
        }
        if (const auto set = given->find(variable); set != given->end()) {
          return std::string_view(set->second.value);
        }
        if (const auto set = rule.defaults.find(variable);
            set != rule.defaults.end()) {
          return std::string_view(set->second.value);
        }
        unset = variable;
        return std::string_view();
      });
      if (!unset.empty()) {
        reject(
            std::format("`{}` uses `{}` without setting `${}`, which has "
                        "no default",
                        left, word, unset));
        continue;
      }
      action = rule.action;
      argument = trim(expanded);
    } else {
      for (const Line& binding : under) {
        reject_at(binding.number,
                  std::format("`{}` does not use a rule, so has no variables "
                              "to set",
                              left));
      }
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
    if (Manifest::is_report(*action)) {
      if (!argument.empty()) {
        reject(
            std::format("`{}` is assigned `{}`, which takes no "
                        "argument, but was given `{}`",
                        left, word, argument));
        continue;
      }
    } else if (argument.empty()) {
      reject(std::format("`{}` has no {}", left, argument_of(*action)));
      continue;
    }

    const std::optional<std::filesystem::path> output =
        to_inside_path(left.substr(2));

    // A `run` or `capture` command is taken verbatim. `copy` names a file
    // instead, held to the same shape as an output - inside the directory, no
    // escaping - so every copy's source is one the build can watch, and stored
    // normalised. One under `@/` is another output.
    std::string command(argument);
    std::optional<std::filesystem::path> copied_output;

    // A `*` in the output makes the rule a wildcard, which needs source files
    // to match: those of the first word of the command to hold a `*`, or of a
    // copy's path.
    const bool wildcard = left.contains('*');
    std::filesystem::path wildcard_source;
    bool over_outputs = false;
    if (wildcard) {
      if (Manifest::is_report(*action)) {
        reject(std::format("`{}` is a wildcard, which `{}` cannot be", left,
                           word));
        continue;
      }
      const std::string_view pattern = *action == Manifest::Action::Copy
                                           ? argument
                                           : first_wildcard_word(argument);
      if (pattern.empty()) {
        reject(
            std::format("`{}` is a wildcard, but its command names no "
                        "source files with a `*`",
                        left));
        continue;
      }
      over_outputs = pattern.starts_with("@/");
      const std::optional<std::filesystem::path> source =
          pattern.find_first_of("\"'") != std::string_view::npos
              ? std::nullopt
              : to_inside_path(over_outputs ? pattern.substr(2) : pattern);
      if (!source.has_value() || !is_wildcard_pattern(*source)) {
        reject(std::format(
            "`{}` takes its sources from `{}`, which must be an unquoted path "
            "inside {} with a single `*`, in its file name",
            left, pattern, over_outputs ? "`@/`" : "the manifest's directory"));
        continue;
      }
      wildcard_source = *source;
    }

    if (wildcard && *action == Manifest::Action::Copy) {
      // Already held to a copy's shape; what it copies is declared by
      // whatever it matches.
      command = (over_outputs ? "@/" : "") + wildcard_source.generic_string();
    } else if (*action == Manifest::Action::Copy) {
      const bool generated = argument.starts_with("@/");
      const std::optional<std::filesystem::path> source =
          to_inside_path(generated ? argument.substr(2) : argument);
      if (!source.has_value()) {
        reject(std::format(
            "`{}` copies `{}`, which does not name a file "
            "inside {}",
            left, argument, generated ? "`@/`" : "the manifest's directory"));
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

    if (wildcard && !is_wildcard_pattern(*output)) {
      reject(
          std::format("`{}` must have a single `*`, in its file name", left));
      continue;
    }

    if (const auto it = files.find(*output); it != files.end()) {
      reject(
          std::format("`{}` is already declared on line {}", left, it->second));
      continue;
    }
    if (const auto it = directories.find(*output); it != directories.end()) {
      reject(
          std::format("`{}` is a directory of the output declared on line {}",
                      left, it->second));
      continue;
    }
    std::optional<std::size_t> clash;
    for (std::filesystem::path parent = output->parent_path(); !parent.empty();
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

    if (!wildcard) {
      files.emplace(*output, number);
    }
    for (std::filesystem::path parent = output->parent_path(); !parent.empty();
         parent = parent.parent_path()) {
      directories.emplace(parent, number);
    }
    if (copied_output.has_value()) {
      copies.push_back({.rule = manifest.m_rules.size(),
                        .line = number,
                        .left = std::string(left),
                        .source = std::move(*copied_output)});
    }
    manifest.m_rules.push_back({.output = *output,
                                .action = *action,
                                .command = std::move(command),
                                .wildcard_source = std::move(wildcard_source),
                                .wildcard_over_outputs = over_outputs,
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

  // A wildcard over outputs must not be able to feed its own sources, directly
  // or through others, or its outputs would never end: `feeds[a][b]` once
  // something rule `a` could produce is something rule `b` could match, then
  // closed over every chain of such rules.
  const std::span<const Rule> rules = manifest.m_rules;
  std::vector<std::vector<bool>> feeds(rules.size(),
                                       std::vector<bool>(rules.size()));
  for (std::size_t a = 0; a < rules.size(); ++a) {
    for (std::size_t b = 0; b < rules.size(); ++b) {
      feeds[a][b] =
          rules[a].is_wildcard() && rules[b].wildcard_over_outputs &&
          rules[a].output.parent_path() ==
              rules[b].wildcard_source.parent_path() &&
          NamePattern(rules[a].output.filename().generic_string())
              .overlaps(NamePattern(
                  rules[b].wildcard_source.filename().generic_string()));
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
