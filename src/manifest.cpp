// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
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

}  // namespace

Manifest Manifest::parse(std::string_view text) {
  Manifest manifest;

  // Where each accepted output, and each directory above one, was declared, so
  // a later rule cannot reuse a path as both a file and a directory.
  std::map<std::filesystem::path, std::size_t> files;
  std::map<std::filesystem::path, std::size_t> directories;

  // Each accepted rule that copies another output, checked against every
  // declared output once all are known.
  struct CopyOfOutput {
    std::size_t rule;
    std::size_t line;
    std::string left;
    std::filesystem::path source;
  };
  std::vector<CopyOfOutput> copies;

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
    // normalised. One under `@/` is another output, which must be declared
    // somewhere in the manifest; that is checked once every line is read.
    std::string command(argument);
    std::optional<std::filesystem::path> copied_output;
    if (*action == Manifest::Action::Copy) {
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

    files.emplace(*output, number);
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
    manifest.m_rules.push_back(
        {.output = *output, .action = *action, .command = std::move(command)});
  }

  // Only now is every output known, since a copy may name one declared after
  // it. Each copy of an undeclared one is dropped, last first so the indices
  // of those before it still hold, and reported in line with the rest.
  bool dropped = false;
  for (const CopyOfOutput& copy : std::views::reverse(copies)) {
    if (!files.contains(copy.source)) {
      manifest.m_rules.erase(manifest.m_rules.begin() +
                             static_cast<std::ptrdiff_t>(copy.rule));
      manifest.m_errors.push_back(
          {.line = copy.line,
           .message = std::format("`{}` copies `@/{}`, which is not declared",
                                  copy.left, copy.source.generic_string())});
      dropped = true;
    }
  }
  if (dropped) {
    std::ranges::stable_sort(manifest.m_errors, {}, &Manifest::Error::line);
  }

  return manifest;
}

}  // namespace makebelieve
