// SPDX-License-Identifier: MIT
#pragma once

#include "buildcache.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace makebelieve {

/// @class DependencyGraph
/// What each output was last built from, as recorded in a @link BuildCache,
/// and printed the way `cargo tree` prints a package's dependencies. Paths are
/// named as the cache names them: `@/` and the path for an output, and the path
/// from the manifest's directory for anything else.
class DependencyGraph {
 public:
  /// What comes before each path printed.
  enum class Prefix : std::uint8_t {
    indent,  // a tree, each path beneath what it is a dependency of
    depth,   // how many steps it is from the path asked about
    none,    // nothing: a sorted list of each path once, without those asked
             // about unless reached from another
  };

  /// What a tree's lines are drawn with.
  enum class Charset : std::uint8_t {
    utf8,   // box-drawing characters
    ascii,  // `|`, `-` and a backtick, for wherever those would not show
  };

  /// What to print.
  struct Options {
    /// Print what depends on each path, rather than what it depends on.
    bool invert = false;

    /// Go at most this many steps from each path; all the way if not given.
    std::optional<std::size_t> depth;

    Prefix prefix = Prefix::indent;

    /// Repeat what is beneath a path each time it appears, rather than once
    /// with `(*)` marking the rest.
    bool no_dedupe = false;

    /// Include directories only looked at and paths found missing, which are
    /// left out of what a path depends on otherwise.
    bool all = false;

    Charset charset = Charset::utf8;
  };

  /// A graph of @a records.
  explicit DependencyGraph(std::span<const BuildCache::Record> records);

  /// What @a options asks for about @a paths, or a message saying which of
  /// them there is nothing recorded for.
  [[nodiscard]] std::expected<std::string, std::string> print(
      std::span<const std::string> paths,
      const Options& options) const;

 private:
  struct Edge {
    std::size_t node;
    BuildCache::Input::Kind kind;
  };

  struct Node {
    std::string name;
    bool built = false;
    std::vector<Edge> inputs;
    std::vector<Edge> dependents;
  };

  std::vector<Node> m_nodes;
  std::map<std::filesystem::path, std::size_t> m_by_key;

  // The node named @a name, added if it is new.
  std::size_t intern(const std::filesystem::path& name);

  // The edges to follow from @a node.
  [[nodiscard]] std::vector<Edge> edges(std::size_t node,
                                        const Options& options) const;

  void print_tree(std::size_t root,
                  const Options& options,
                  std::string& out) const;

  void print_list(std::span<const std::size_t> roots,
                  const Options& options,
                  std::string& out) const;
};

}  // namespace makebelieve
