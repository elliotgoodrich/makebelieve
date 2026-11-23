// SPDX-License-Identifier: MIT
#include "dependencygraph.hpp"

#include "filesystemutil.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace makebelieve {

namespace {

// @a path as printed: UTF-8, with `/` between parts.
std::string to_text(const std::filesystem::path& path) {
  const std::u8string text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path from_text(std::string_view text) {
  return {std::u8string(reinterpret_cast<const char8_t*>(text.data()),
                        text.size())};
}

// @a path as printed, with the root of the source - the empty path - as `.`.
std::string display(const std::filesystem::path& path) {
  return path.empty() ? "." : to_text(path);
}

// The path recorded for what @a text names: normalized, without a trailing
// separator, and with `.` as the empty path of the source's root.
std::filesystem::path recorded_path(std::string_view text) {
  std::filesystem::path path = from_text(text).lexically_normal();
  if (!path.has_filename()) {
    path = path.parent_path();
  }
  return path == "." ? std::filesystem::path() : path;
}

// What to say after a path about how it was seen, if it is not simply a file.
std::string_view label(BuildCache::Input::Kind kind) {
  switch (kind) {
    case BuildCache::Input::Kind::file:
    case BuildCache::Input::Kind::output:
      return "";
    case BuildCache::Input::Kind::directory:
      return " (directory)";
    case BuildCache::Input::Kind::listing:
      return " (listed)";
    case BuildCache::Input::Kind::missing:
      return " (missing)";
  }
  return "";
}

// The pieces a tree is drawn from.
struct TreeLines {
  std::string_view branch;  // to a child with more after it
  std::string_view last;    // to the last child
  std::string_view pipe;    // beneath a child with more after it
  std::string_view blank;   // beneath the last child
};

// Spelled as UTF-8 bytes, as the compiler's execution charset may not be UTF-8:
// U+251C, U+2514, U+2502 and U+2500.
constexpr TreeLines k_utf8_lines{
    .branch = "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ",
    .last = "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ",
    .pipe = "\xE2\x94\x82   ",
    .blank = "    ",
};

constexpr TreeLines k_ascii_lines{
    .branch = "|-- ",
    .last = "`-- ",
    .pipe = "|   ",
    .blank = "    ",
};

}  // namespace

DependencyGraph::DependencyGraph(std::span<const BuildCache::Record> records) {
  for (const BuildCache::Record& record : records) {
    const std::size_t output =
        intern(std::filesystem::path("@") / record.command.output);
    m_nodes[output].built = true;
    for (const BuildCache::Input& input : record.inputs) {
      const std::size_t node = intern(input.path);
      m_nodes[output].inputs.push_back({.node = node, .kind = input.kind});
      m_nodes[node].dependents.push_back({.node = output, .kind = input.kind});
    }
  }
  const auto by_name = [this](const Edge& left, const Edge& right) {
    return m_nodes[left.node].name < m_nodes[right.node].name;
  };
  for (Node& node : m_nodes) {
    std::ranges::sort(node.inputs, by_name);
    std::ranges::sort(node.dependents, by_name);
  }
}

std::size_t DependencyGraph::intern(const std::filesystem::path& name) {
  const auto [it, added] =
      m_by_key.try_emplace(FileSystemUtil::path_key(name), m_nodes.size());
  if (added) {
    m_nodes.push_back(Node{.name = display(name)});
  }
  return it->second;
}

std::vector<DependencyGraph::Edge> DependencyGraph::edges(
    std::size_t node,
    const Options& options) const {
  if (options.invert) {
    return m_nodes[node].dependents;
  }
  std::vector<Edge> shown = m_nodes[node].inputs;
  if (!options.all) {
    std::erase_if(shown, [](const Edge& edge) {
      return edge.kind == BuildCache::Input::Kind::directory ||
             edge.kind == BuildCache::Input::Kind::missing;
    });
  }
  return shown;
}

std::expected<std::string, std::string> DependencyGraph::print(
    std::span<const std::string> paths,
    const Options& options) const {
  std::vector<std::size_t> roots;
  for (const std::string& path : paths) {
    const std::filesystem::path name = recorded_path(path);
    const auto it = m_by_key.find(FileSystemUtil::path_key(name));
    if (options.invert) {
      if (it == m_by_key.end()) {
        return std::unexpected(std::format(
            "nothing built so far depends on [{}]; outputs are named `@/path` "
            "and sources by their path from the manifest's directory",
            display(name)));
      }
    } else if (it == m_by_key.end() || !m_nodes[it->second].built) {
      return std::unexpected(std::format(
          "[{}] is not an output that has been built, and what an output "
          "depends on is only known once it has; outputs are named `@/path`",
          display(name)));
    }
    roots.push_back(it->second);
  }

  std::string out;
  if (options.prefix == Prefix::none) {
    print_list(roots, options, out);
    return out;
  }
  for (std::size_t i = 0; i < roots.size(); ++i) {
    if (i != 0) {
      out += '\n';
    }
    print_tree(roots[i], options, out);
  }
  return out;
}

void DependencyGraph::print_tree(std::size_t root,
                                 const Options& options,
                                 std::string& out) const {
  // A stack rather than recursion, as a chain of outputs can be any length.
  struct Frame {
    std::size_t node;
    std::vector<Edge> children;
    std::string prefix;
    std::size_t next = 0;
  };

  const bool indent = options.prefix == Prefix::indent;
  const TreeLines& lines =
      options.charset == Charset::utf8 ? k_utf8_lines : k_ascii_lines;
  const auto within_depth = [&options](std::size_t depth) {
    return !options.depth.has_value() || depth < *options.depth;
  };

  // The shallowest depth each node's children were printed from. A node first
  // met near the depth limit is printed again in full if met nearer the root.
  std::unordered_map<std::size_t, std::size_t> expanded;
  std::unordered_set<std::size_t> ancestors{root};
  std::vector<Frame> stack;

  out += indent ? m_nodes[root].name : std::format("0{}", m_nodes[root].name);
  out += '\n';
  stack.push_back(Frame{
      .node = root,
      .children = within_depth(0) ? edges(root, options) : std::vector<Edge>{},
      .prefix = {}});
  if (!stack.back().children.empty()) {
    expanded.emplace(root, 0);
  }

  while (!stack.empty()) {
    if (stack.back().next == stack.back().children.size()) {
      ancestors.erase(stack.back().node);
      stack.pop_back();
      continue;
    }
    Frame& top = stack.back();
    const Edge edge = top.children[top.next++];
    const bool last = top.next == top.children.size();
    const std::size_t depth = stack.size();

    std::vector<Edge> children;
    if (within_depth(depth)) {
      children = edges(edge.node, options);
    }
    const auto seen = expanded.find(edge.node);
    const bool repeated =
        !children.empty() && (ancestors.contains(edge.node) ||
                              (!options.no_dedupe && seen != expanded.end() &&
                               seen->second <= depth));

    if (indent) {
      out += top.prefix;
      out += last ? lines.last : lines.branch;
    } else {
      out += std::to_string(depth);
    }
    out += m_nodes[edge.node].name;
    if (!options.invert) {
      out += label(edge.kind);
    }
    out += repeated ? " (*)\n" : "\n";

    if (!repeated && !children.empty()) {
      expanded.insert_or_assign(edge.node, depth);
      ancestors.insert(edge.node);
      std::string prefix = top.prefix;
      prefix += last ? lines.blank : lines.pipe;
      // Invalidates top.
      stack.push_back(Frame{.node = edge.node,
                            .children = std::move(children),
                            .prefix = std::move(prefix)});
    }
  }
}

void DependencyGraph::print_list(std::span<const std::size_t> roots,
                                 const Options& options,
                                 std::string& out) const {
  // Breadth first, so each node is first met at its shallowest depth.
  std::deque<std::pair<std::size_t, std::size_t>> queue;
  std::unordered_set<std::size_t> visited;
  std::set<std::string> names;
  for (const std::size_t root : roots) {
    if (visited.insert(root).second) {
      queue.emplace_back(root, 0);
    }
  }
  while (!queue.empty()) {
    const auto [node, depth] = queue.front();
    queue.pop_front();
    if (options.depth.has_value() && depth >= *options.depth) {
      continue;
    }
    for (const Edge& edge : edges(node, options)) {
      names.insert(m_nodes[edge.node].name);
      if (visited.insert(edge.node).second) {
        queue.emplace_back(edge.node, depth + 1);
      }
    }
  }
  for (const std::string& name : names) {
    out += name;
    out += '\n';
  }
}

}  // namespace makebelieve
