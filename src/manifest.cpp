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
#include <unordered_map>
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
              .wildcard_over_outputs = false};
}

Manifest Manifest::parse(std::string_view text) {
  Manifest manifest;

  // Where each accepted output, and each directory above one, was declared, so
  // a later rule cannot reuse a path as both a file and a directory.
  std::map<std::filesystem::path, std::size_t> files;
  std::map<std::filesystem::path, std::size_t> directories;

  // What each accepted wildcard's outputs end in - whatever follows its `*` -
  // by the directory they are in. Nothing else there may end in it: sooner or
  // later a source file would turn up to make the wildcard produce that name.
  struct BannedSuffix {
    std::string suffix;
    std::size_t line;
  };
  std::unordered_map<std::filesystem::path, std::vector<BannedSuffix>>
      banned_suffixes;

  // The suffix banned in @a path's directory that its name ends in, if any.
  const auto banned_suffix =
      [&](const std::filesystem::path& path) -> const BannedSuffix* {
    const auto banned = banned_suffixes.find(path.parent_path());
    if (banned == banned_suffixes.end()) {
      return nullptr;
    }
    const std::string name = path.filename().generic_string();
    const auto it =
        std::ranges::find_if(banned->second, [&](const BannedSuffix& other) {
          return name.ends_with(other.suffix);
        });
    return it == banned->second.end() ? nullptr : &*it;
  };

  // Accepted copies of outputs, checked once every output is known.
  struct CopyOfOutput {
    std::size_t rule;
    std::size_t line;
    std::string left;
    std::filesystem::path source;
  };
  std::vector<CopyOfOutput> copies;

  // The line each accepted rule was declared on.
  std::vector<std::size_t> lines;

  std::size_t start = 0;
  for (std::size_t number = 1; start <= text.size(); ++number) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end =
        newline == std::string_view::npos ? text.size() : newline;
    const std::string_view line = trim(text.substr(start, end - start));
    start = end + 1;

    if (line.empty() || line.front() == '#') {
      continue;
    }

    const auto reject = [&](std::string message) {
      manifest.m_errors.push_back(
          {.line = number, .message = std::move(message)});
    };

    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      reject("expected `@/<output> = <action> <command>`");
      continue;
    }

    const std::string_view left = trim(line.substr(0, equals));
    const std::string_view value = trim(line.substr(equals + 1));
    if (!left.starts_with("@/")) {
      reject(std::format("output `{}` must start with `@/`", left));
      continue;
    }

    // An output is assigned an action and what that action applies to; the
    // rest of the line after the action is that argument.
    const std::string_view word = first_word(value);
    const std::optional<Manifest::Action> action = to_action(word);
    if (!action.has_value()) {
      reject(
          std::format("`{}` must be assigned `run <command>`, "
                      "`capture <command>`, `copy <path>` or `tracing`",
                      left));
      continue;
    }
    const std::string_view argument = trim(value.substr(word.size()));
    if (*action == Manifest::Action::Tracing) {
      if (!argument.empty()) {
        reject(
            std::format("`{}` is assigned `tracing`, which takes no "
                        "argument, but was given `{}`",
                        left, argument));
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
      if (*action == Manifest::Action::Tracing) {
        reject(
            std::format("`{}` is a wildcard, which `tracing` cannot be", left));
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

    // Nor may anything end in a suffix banned where it is: not one of this
    // output's directories...
    const BannedSuffix* banned = nullptr;
    for (std::filesystem::path parent = output->parent_path();
         banned == nullptr && !parent.empty(); parent = parent.parent_path()) {
      banned = banned_suffix(parent);
    }
    if (banned != nullptr) {
      reject(
          std::format("`{}` is inside a directory ending in `{}`, which "
                      "only files of the wildcard on line {} may",
                      left, banned->suffix, banned->line));
      continue;
    }

    // ...nor the output itself. A wildcard bans its own suffix, so nothing
    // already there may end in that either.
    const std::filesystem::path directory = output->parent_path();
    if (!wildcard) {
      if (const BannedSuffix* suffix = banned_suffix(*output)) {
        reject(
            std::format("`{}` ends in `{}`, which only outputs of the "
                        "wildcard on line {} may",
                        left, suffix->suffix, suffix->line));
        continue;
      }
    } else {
      const std::string suffix =
          NamePattern(output->filename().generic_string()).suffix;
      const auto ends_in_it = [&](const auto& declared) {
        return declared.first.parent_path() == directory &&
               declared.first.filename().generic_string().ends_with(suffix);
      };
      if (const auto it = std::ranges::find_if(files, ends_in_it);
          it != files.end()) {
        reject(
            std::format("`{}` bans outputs ending in `{}` from its "
                        "directory, but line {} declares one",
                        left, suffix, it->second));
        continue;
      }
      if (const auto it = std::ranges::find_if(directories, ends_in_it);
          it != directories.end()) {
        reject(
            std::format("`{}` bans names ending in `{}` from its "
                        "directory, but the output on line {} is inside "
                        "one",
                        left, suffix, it->second));
        continue;
      }
      // Each wildcard's outputs would end in the other's suffix if either
      // suffix ends in the other.
      std::vector<BannedSuffix>& siblings = banned_suffixes[directory];
      if (const auto it =
              std::ranges::find_if(siblings,
                                   [&](const BannedSuffix& other) {
                                     return suffix.ends_with(other.suffix) ||
                                            other.suffix.ends_with(suffix);
                                   });
          it != siblings.end()) {
        reject(std::format(
            "`{}` and the wildcard on line {} could both "
            "produce outputs ending in `{}`",
            left, it->line,
            suffix.size() > it->suffix.size() ? suffix : it->suffix));
        continue;
      }
      siblings.push_back({.suffix = suffix, .line = number});
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
    lines.push_back(number);
    manifest.m_rules.push_back({.output = *output,
                                .action = *action,
                                .command = std::move(command),
                                .wildcard_source = std::move(wildcard_source),
                                .wildcard_over_outputs = over_outputs});
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
          {.line = lines[rule],
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
  if (!dropped.empty()) {
    std::ranges::stable_sort(manifest.m_errors, {}, &Manifest::Error::line);
  }

  return manifest;
}

}  // namespace makebelieve
