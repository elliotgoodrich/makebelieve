// SPDX-License-Identifier: MIT
#include "manifest.hpp"

#include <cstddef>
#include <filesystem>
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

}  // namespace

Manifest Manifest::parse(std::string_view text) {
  Manifest manifest;

  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end =
        newline == std::string_view::npos ? text.size() : newline;
    const std::string_view line = trim(text.substr(start, end - start));
    start = end + 1;

    if (line.empty() || line.front() == '#') {
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

    manifest.m_rules.push_back(
        {.output = std::filesystem::path(output).lexically_normal(),
         .command = std::string(command)});
  }

  return manifest;
}

}  // namespace makebelieve
