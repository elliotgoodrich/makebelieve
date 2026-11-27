// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "buildcache.hpp"
#include "buildcoordinator.hpp"
#include "buildpermits.hpp"
#include "directorytreeutil.hpp"
#include "filesystemutil.hpp"
#include "inmemorydirectorytree.hpp"
#include "manifest.hpp"
#include "openedfileutil.hpp"
#include "processinfo.hpp"
#include "scopeguard.hpp"
#include "shellrunner.hpp"
#include "tracer.hpp"

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace makebelieve {

namespace {

// The default entry point.
constexpr std::string_view k_manifest_name = "build.makebelieve";

// The content an output holds before it has ever been built: a single null
// byte. Viewing a named char rather than a string literal keeps the length and
// the storage in step (a `string_view("", 1)` reads as out-of-bounds).
constexpr char k_null_byte = '\0';
constexpr std::string_view k_placeholder_content(&k_null_byte, 1);

// Creates every missing directory on the way to @a output's parent inside
// @a tree, so write_file()'s precondition that the parent exists holds even
// for a nested output like `@/out/foo.o`.
void ensure_parent_directories(InMemoryDirectoryTree& tree,
                               const std::filesystem::path& output) {
  std::filesystem::path directory;
  for (const std::filesystem::path& part : output.parent_path()) {
    directory /= part;
    if (!tree.status(directory).has_value()) {
      tree.make_directory(directory);
    }
  }
}

// Removes @a output from @a tree along with any directories that removing it
// leaves empty, undoing ensure_parent_directories().
void remove_with_empty_parents(InMemoryDirectoryTree& tree,
                               const std::filesystem::path& output) {
  if (!tree.status(output).has_value()) {
    return;
  }
  tree.remove(output);
  for (std::filesystem::path directory = output.parent_path();
       !directory.empty(); directory = directory.parent_path()) {
    const auto children = tree.ls(directory);
    if (!children.has_value() || !children->empty()) {
      return;
    }
    tree.remove(directory);
  }
}

constexpr std::string_view k_generated_prefix = "@/";

// A file a runner left, which the tree has taken over: deleted when this goes,
// unless something - the cache - takes it first.
class ScratchFile {
  std::filesystem::path m_path;

 public:
  ScratchFile() = default;
  explicit ScratchFile(std::filesystem::path path) : m_path(std::move(path)) {}
  ~ScratchFile() {
    if (!m_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(m_path, ignored);
    }
  }
  ScratchFile(ScratchFile&& other) noexcept
      : m_path(std::exchange(other.m_path, {})) {}
  ScratchFile& operator=(ScratchFile other) noexcept {
    std::swap(m_path, other.m_path);
    return *this;
  }
  ScratchFile(const ScratchFile&) = delete;

  [[nodiscard]] bool empty() const { return m_path.empty(); }

  // Hands the file over, so it is no longer deleted here.
  [[nodiscard]] std::filesystem::path release() {
    return std::exchange(m_path, {});
  }
};

// What BuildDirectoryTreeTestUtil::before_next_status_write() was last given,
// and not yet taken.
std::mutex g_status_write_hook_mutex;
std::function<void()> g_before_status_write;

// Takes the hook to call before the next status announcement writes, if any.
std::function<void()> take_before_status_write() {
  const std::lock_guard lock(g_status_write_hook_mutex);
  return std::exchange(g_before_status_write, nullptr);
}

// What BuildDirectoryTreeTestUtil::before_next_publish() was last given, and
// not yet taken.
std::function<void()> g_before_publish;

// Takes the hook to call before the next output is put in place, if any.
std::function<void()> take_before_publish() {
  const std::lock_guard lock(g_status_write_hook_mutex);
  return std::exchange(g_before_publish, nullptr);
}

// The tree whose layout lock this thread holds, if any. A subscriber told of
// a change to its outputs is told while that lock is held, so must not wait on
// it again - or on another tree's, which that tree's subscribers may hold
// while waiting on this one's.
thread_local const void* t_layout_holder = nullptr;

// The layout lock of @a tree, held - or, given `std::try_to_lock`, held only
// if it was free - noting that this thread holds it.
class LayoutLock {
  std::unique_lock<std::mutex> m_lock;
  const void* m_previous = nullptr;

 public:
  LayoutLock(std::mutex& mutex, const void* tree)
      : m_lock(mutex), m_previous(std::exchange(t_layout_holder, tree)) {}
  LayoutLock(std::mutex& mutex, const void* tree, std::try_to_lock_t)
      : m_lock(mutex, std::try_to_lock) {
    if (m_lock.owns_lock()) {
      m_previous = std::exchange(t_layout_holder, tree);
    }
  }
  ~LayoutLock() {
    if (m_lock.owns_lock()) {
      t_layout_holder = m_previous;
    }
  }

  [[nodiscard]] bool held() const { return m_lock.owns_lock(); }

  LayoutLock(const LayoutLock&) = delete;
  LayoutLock& operator=(const LayoutLock&) = delete;
  LayoutLock(LayoutLock&&) = delete;
  LayoutLock& operator=(LayoutLock&&) = delete;
};

// @a output as an input of the builds that read it, kept apart from source
// files by its `@/`.
std::filesystem::path as_input(const std::filesystem::path& output) {
  return output.empty() ? std::filesystem::path("@")
                        : std::filesystem::path("@") / output;
}

// Whether @a path is @a ancestor or lies beneath it. Every path lies beneath
// the root, the empty path.
bool is_within(const std::filesystem::path& path,
               const std::filesystem::path& ancestor) {
  return std::ranges::mismatch(ancestor, path).in1 == ancestor.end();
}

// Whether @a error says there is nothing at a path: neither it nor, perhaps,
// a directory on the way to it.
bool is_missing(const std::error_code& error) {
  return error == std::errc::no_such_file_or_directory ||
         error == std::errc::not_a_directory;
}

// The name of @a action as a manifest spells it.
std::string_view to_string(Manifest::Action action) {
  switch (action) {
    case Manifest::Action::Run:
      return "run";
    case Manifest::Action::Capture:
      return "capture";
    case Manifest::Action::Copy:
      return "copy";
    case Manifest::Action::Tracing:
      return "tracing";
    case Manifest::Action::Status:
      return "status";
  }
  return "unknown";
}

// The content of a `tracing` output: the trace so far, or an empty one when
// nothing is being traced.
std::string trace_snapshot() {
  return g_tracer != nullptr ? g_tracer->snapshot() : std::string("[]\n");
}

// Appends @a output as a manifest names it, `@/` and all.
void append_output(std::string& report, const std::filesystem::path& output) {
  report += k_generated_prefix;
  report += output.generic_string();
}

// Appends @a command as its manifest line spells it after the `=`.
void append_command(std::string& report, const Command& command) {
  report += to_string(command.action);
  if (!command.text.empty()) {
    report += ' ';
    report += command.text;
  }
}

// Appends @a text with each line indented, and every line ended by a newline
// alone. Blank lines at its end are left out, so they do not part one entry
// of a report from the next.
void append_indented(std::string& report, std::string_view text) {
  const std::size_t last = text.find_last_not_of("\r\n");
  text = last == std::string_view::npos ? std::string_view()
                                        : text.substr(0, last + 1);
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view()
                                         : text.substr(end + 1);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (!line.empty()) {
      report += "    ";
      report += line;
    }
    report += '\n';
  }
}

// Appends why a build failed: its exit status if its command exited with one,
// otherwise the error itself.
void append_reason(std::string& report, const std::error_code& error) {
  if (error.category() == exit_status_category()) {
    report += "exit ";
    report += std::to_string(error.value());
  } else {
    report += error.message();
  }
}

// Reads the rules of the manifest in @a source, or describes - one problem per
// line - why the manifest could not be read or has lines it cannot accept. No
// manifest at all means no rules.
std::expected<std::vector<Manifest::Rule>, std::string> read_rules(
    const DirectoryTree& source) {
  const std::expected<std::string, std::error_code> manifest =
      DirectoryTreeUtil::wait_read(
          DirectoryTreeUtil::read_whole(source, k_manifest_name));
  if (!manifest.has_value()) {
    if (manifest.error() == std::errc::no_such_file_or_directory) {
      return {};
    }
    return std::unexpected(
        std::format("{}: {}", k_manifest_name, manifest.error().message()));
  }

  const Manifest parsed = Manifest::parse(*manifest);
  if (!parsed.errors().empty()) {
    std::string problems;
    for (const Manifest::Error& error : parsed.errors()) {
      if (!problems.empty()) {
        problems += '\n';
      }
      problems +=
          std::format("{}:{}: {}", k_manifest_name, error.line, error.message);
    }
    return std::unexpected(std::move(problems));
  }

  return std::vector<Manifest::Rule>(parsed.rules().begin(),
                                     parsed.rules().end());
}

// One rule's claim on a path, as a `status` output names it when more than one
// rule claims that path.
struct Claim {
  // What the rule would build. Its output is the claimed path, or a directory
  // of it - one the rule would make a file, where another rule needs a
  // directory.
  Command command;

  // The line of the manifest declaring the rule, and the rule as it spells it.
  std::size_t line = 0;
  std::string rule;

  // The source file - or `@/` and the output - a pattern matched to claim
  // the path. Empty for a simple rule.
  std::filesystem::path matched;

  [[nodiscard]] friend bool operator==(const Claim&, const Claim&) = default;
};

// What a manifest's rules declare, given the files their patterns match.
struct Expansion {
  // Each path exactly one rule claims, mapped to the command that builds it.
  std::map<std::filesystem::path, Command> commands;

  // Each path more than one rule claims, mapped to every claim on it in line
  // order. Nothing builds it, and opening it fails.
  std::map<std::filesystem::path, std::vector<Claim>> conflicts;
};

// Calls @a visit with the path of each file beneath @a directory of
// @a source - only those directly in it unless @a recursive - relative to
// @a source's root. A directory that is missing or cannot be listed holds
// nothing.
void for_each_file(const DirectoryTree& source,
                   const std::filesystem::path& directory,
                   bool recursive,
                   std::invocable<const std::filesystem::path&> auto&& visit) {
  std::vector<std::filesystem::path> pending = {directory};
  while (!pending.empty()) {
    const std::filesystem::path listed = std::move(pending.back());
    pending.pop_back();
    const auto entries = source.ls(listed);
    if (!entries.has_value()) {
      continue;
    }
    for (const TreeEntry& entry : *entries) {
      std::filesystem::path path =
          listed.empty() ? entry.name : listed / entry.name;
      if (std::holds_alternative<FileInfo>(entry.info)) {
        visit(path);
      } else if (recursive) {
        pending.push_back(std::move(path));
      }
    }
  }
}

// Finds every path @a rules claim, a pattern rule claiming one for every file
// in @a source its pattern matches right now and every output it matches.
// Two rules may claim the same path, or one may claim a path as a file where
// another claims something inside it: each such path is a conflict, and every
// other path's one claim declares it.
Expansion expand_rules(const DirectoryTree& source,
                       const std::vector<Manifest::Rule>& rules) {
  std::map<std::filesystem::path, std::vector<Claim>> claims;
  const auto claim = [&](const Manifest::Rule& rule,
                         const Manifest::Rule& simple,
                         std::filesystem::path matched) {
    claims[simple.output].push_back(
        Claim{.command = Command{.output = simple.output,
                                 .action = simple.action,
                                 .text = simple.command},
              .line = rule.line,
              .rule = rule.text,
              .matched = std::move(matched)});
  };
  for (const Manifest::Rule& rule : rules) {
    if (!rule.is_pattern()) {
      claim(rule, rule, {});
      continue;
    }
    if (rule.pattern->over_outputs) {
      continue;  // below, once there are outputs to match
    }
    for_each_file(
        source, rule.pattern->directory, rule.pattern->recursive,
        [&](const std::filesystem::path& path) {
          if (const std::optional<Manifest::Rule> matched =
                  Manifest::instantiate(rule, path.generic_string())) {
            claim(rule, *matched, path);
          }
        });
  }

  // A pattern over outputs matches every path claimed so far, which may be
  // what lets another match, so go round until nothing is added - matching
  // each path once per rule. The manifest rejects patterns that could feed
  // themselves, so this ends.
  std::set<std::pair<std::size_t, std::filesystem::path>> tried;
  for (bool added = true; added;) {
    added = false;
    for (std::size_t index = 0; index < rules.size(); ++index) {
      const Manifest::Rule& rule = rules[index];
      if (!rule.is_pattern() || !rule.pattern->over_outputs) {
        continue;
      }
      std::vector<std::pair<Manifest::Rule, std::filesystem::path>> matches;
      for (const auto& [output, others] : claims) {
        if (!tried.emplace(index, output).second) {
          continue;
        }
        if (std::optional<Manifest::Rule> matched =
                Manifest::instantiate(rule, output.generic_string())) {
          matches.emplace_back(std::move(*matched), output);
        }
      }
      for (const auto& [matched, output] : matches) {
        claim(rule, matched, std::filesystem::path("@") / output);
        added = true;
      }
    }
  }

  // A path claimed with something claimed inside it is a directory, and each
  // claim on it, which would make it a file, conflicts with every claim
  // inside it.
  std::set<std::filesystem::path> directories;
  for (const auto& [output, others] : claims) {
    for (std::filesystem::path parent = output.parent_path(); !parent.empty();
         parent = parent.parent_path()) {
      if (claims.contains(parent)) {
        directories.insert(parent);
      }
    }
  }

  Expansion expansion;
  for (const auto& [output, own] : claims) {
    if (directories.contains(output)) {
      continue;
    }
    std::vector<Claim> all = own;
    for (std::filesystem::path parent = output.parent_path(); !parent.empty();
         parent = parent.parent_path()) {
      if (directories.contains(parent)) {
        const std::vector<Claim>& above = claims.at(parent);
        all.insert(all.end(), above.begin(), above.end());
      }
    }
    if (all.size() == 1) {
      expansion.commands.emplace(output, std::move(all.front().command));
      continue;
    }
    std::ranges::sort(all, [](const Claim& a, const Claim& b) {
      return std::tie(a.line, a.matched, a.command.output) <
             std::tie(b.line, b.matched, b.command.output);
    });
    expansion.conflicts.emplace(output, std::move(all));
  }
  return expansion;
}

}  // namespace

class BuildDirectoryTree::Impl {
  // The directory structure and file contents we present. Each declared output
  // starts as a placeholder, replaced by real content once its command
  // succeeds - but for a `status` output, which holds its report from the
  // start. Only placeholders and reports, and what a runner gave as bytes,
  // are held in memory: everything else is served from the file it was built
  // into, or restored from, kept open.
  InMemoryDirectoryTree m_structure;

  // The manifest's home, observed for input and manifest changes.
  const DirectoryTree& m_source;

  // Serialises every write to m_structure (placeholders, removals, build
  // results) with the change to m_commands behind it, so a build that finishes
  // just as its rule is removed cannot write the output back. Taken before
  // m_mutex, and held while m_structure notifies subscribers, so a subscriber
  // must not open() through us from its callback.
  std::mutex m_layout_mutex;

  // Guards all the build bookkeeping below, which reads (on filesystem threads)
  // and source-change notifications (on the source's watcher thread) race over.
  // Never held across a call to the runner or m_structure, so a synchronous
  // runner or a subscriber reading back through us cannot deadlock on it.
  std::mutex m_mutex;

  // The rules of the manifest as last accepted, and every directory whose list
  // of children decides which files their patterns match. Touched only by the
  // constructor and the source-change callback, so unguarded.
  std::vector<Manifest::Rule> m_rules;
  std::set<std::filesystem::path> m_pattern_directories;
  // Of those, each that a pattern lists every directory beneath, so a change
  // to any of them may change what it matches.
  std::set<std::filesystem::path> m_recursive_directories;

  // Every declared output mapped to the command that (re)builds it, following
  // the manifest and the files its patterns match as they change.
  std::map<std::filesystem::path, Command> m_commands;

  // Every path more than one rule claims right now, as expand_rules() found
  // it: not in m_commands, never built, shown with its placeholder, and
  // failing whatever opens it.
  std::map<std::filesystem::path, std::vector<Claim>> m_conflicts;

  // Outputs that need (re)building: every output but a `status` output, which
  // nobody builds, starts stale; a successful build clears it, an input change
  // marks it stale again.
  std::set<std::filesystem::path> m_stale;

  // Outputs whose build has started and is awaiting completion, so a second
  // open or a coincident input change does not launch it again. An output
  // both in flight and stale changed after its build started.
  std::set<std::filesystem::path> m_in_flight;

  // Each output's build attempts, as the coordinator knows them: the one
  // under way or last run, and the next, created by an open that arrived
  // after the output changed mid-build and started once that build ends.
  struct Attempts {
    BuildCoordinator::AttemptPtr current;
    BuildCoordinator::AttemptPtr next;

    // The output's count of m_invalidations when `current` started, so its
    // result is kept in the cache only if nothing invalidated it since.
    std::uint64_t invalidations_at_start = 0;
  };
  std::map<std::filesystem::path, Attempts> m_attempts;

  // The outputs each build under way has opened, or looked up or listed,
  // through us, by the output it builds, each with whether it was listed,
  // added to its inputs when it finishes. Tracing sees only the source, so
  // not these.
  std::unordered_map<std::filesystem::path,
                     std::map<std::filesystem::path, bool>>
      m_reads;

  // The builds under way, or finished since, that looked up each output
  // through the mount while it was unbuilt, out of date or building: they saw
  // what it was before, and so go out of date once it is built. A build that
  // goes on to open the output waits for it to be up to date, and so is
  // dropped.
  std::map<std::filesystem::path, std::set<std::filesystem::path>>
      m_saw_out_of_date;

  // Outputs opened at least once, which an input change rebuilds eagerly.
  std::set<std::filesystem::path> m_materialized;

  // Whether every output but a trace counts as opened, as soon as it is
  // declared: set by materialize_all().
  bool m_materialize_all = false;

  // The id of the last build started, which ties the arrow drawn from the open
  // that asked for it to the build itself.
  std::uint64_t m_last_build_id = 0;

  // The rows builds are recorded on, one per build running at once.
  TraceLanePool m_build_lanes{"build"};

  // Guards m_busy_lanes, which is its own concern and never held with another
  // lock.
  std::mutex m_lane_mutex;

  // The row each build under way is being recorded on, so its end lands on the
  // same row its beginning did.
  std::unordered_map<std::filesystem::path, TraceLane> m_busy_lanes;

  // The inputs each built output last read. Kept so a rebuild can refresh
  // m_dependents when an output's set of inputs changes.
  std::map<std::filesystem::path, std::vector<Input>> m_dependencies;

  // The reverse of m_dependencies: each input's FileSystemUtil::path_key
  // mapped to the outputs that read it, and whether each listed it, matched
  // against a change to find what to rebuild. Ordered, so that everything
  // beneath a path is a run starting at it.
  std::map<std::filesystem::path, std::map<std::filesystem::path, bool>>
      m_dependents;

  // How many times each output has gone stale through invalidate(), or had
  // its rule removed: a build started at one count describes the output only
  // while the count stands.
  std::map<std::filesystem::path, std::uint64_t> m_invalidations;

  // The hash of each output's content as served, kept while there is a cache,
  // so a build that reads the output can record what it read.
  std::map<std::filesystem::path, std::string> m_hashes;

  // Where builds are kept across runs; may be null.
  BuildCache* m_cache;

  // Taken around each change to m_cache, before m_mutex, so a build's record
  // is stored only if nothing invalidated the build first, and an
  // invalidation's dropping of records always comes after any such store.
  std::mutex m_cache_mutex;

  CommandRunner m_runner;

  // Used when the tree is given no coordinator: no limit on builds, and no
  // request found to come from any.
  std::optional<exec::static_thread_pool> m_own_workers;
  std::optional<BuildCoordinator> m_own_coordinator;

  // Runs every build and holds every open that waits on one.
  BuildCoordinator* m_coordinator;

  // Told why a changed manifest was rejected; may be empty.
  ManifestErrorHandler m_on_manifest_error;

  // Told of each build that failed; may be empty.
  BuildFailureHandler m_on_build_failure;

  // Why an output's last build failed: what it ran, how that went wrong, and
  // an output it read that had failed, if one did - which for a copy of that
  // output is the reason enough, but for a command may not be, as it may have
  // dealt with that and failed for another reason.
  struct Failure {
    Command command;
    std::error_code error;
    std::string standard_error;
    std::optional<std::filesystem::path> failed_input;
    bool copies_failed_input = false;
  };

  // Every output whose last build failed, until one succeeds or its rule goes.
  std::map<std::filesystem::path, Failure> m_failures;

  // The builds under way that have started running, rather than waiting
  // their turn: a subset of m_in_flight.
  std::set<std::filesystem::path> m_running;

  // The command each build under way runs, as it was when claimed - which a
  // change to its rule mid-build does not change.
  std::map<std::filesystem::path, Command> m_claimed;

  // Every declared `status` output, kept in step with m_commands.
  std::vector<std::filesystem::path> m_status_outputs;

  // How many times what a `status` output shows may have changed, and the
  // count that the `status` outputs show: behind it, they are out of date.
  std::uint64_t m_status_version = 0;
  std::uint64_t m_status_written = 0;

  // Whether a change was announced, by writing it, since a `status` output was
  // last read: until it is read again, further changes are not.
  bool m_status_announced = false;

  // Subscribers, as subscribe_to_changes() was given them: told by the tree
  // itself of each change to its `status` outputs, once it holds no lock, so
  // that they may read back whatever they like - any tree's `status` outputs
  // included - rather than by m_structure, which tells them with the layout
  // lock held.
  std::mutex m_subscribers_mutex;
  std::list<std::function<void(const DirectoryTreeDiff&)>> m_subscribers;

  // Why the latest change to the manifest was rejected, one problem per line,
  // or empty if it was accepted.
  std::string m_manifest_problems;

  // Every build under way. Destruction requests a stop through it, telling
  // those builds their results are no longer wanted, then joins it, so none
  // completes into a tree that is gone.
  stdexec::counting_scope m_builds;

  // Drains the source-change callback on teardown: the callback holds the gate
  // while it touches our state and bails if closed, and the destructor closes
  // it (waiting out any callback in progress) before destroying that state. A
  // shared_ptr so the callback can lock it safely even after we are gone.
  struct Gate {
    std::mutex mutex;
    bool open = true;
  };
  std::shared_ptr<Gate> m_gate = std::make_shared<Gate>();

  // Observes the source for input changes for as long as this tree lives.
  std::optional<Subscription> m_source_subscription;

 public:
  Impl(const DirectoryTree& source,
       CommandRunner runner,
       BuildCoordinator* coordinator,
       ManifestErrorHandler on_manifest_error,
       BuildCache* cache,
       BuildFailureHandler on_build_failure)
      : m_source(source),
        m_cache(cache),
        m_runner(std::move(runner)),
        m_coordinator(coordinator),
        m_on_manifest_error(std::move(on_manifest_error)),
        m_on_build_failure(std::move(on_build_failure)) {
    if (m_coordinator == nullptr) {
      m_coordinator = &m_own_coordinator.emplace(
          unlimited_permits(), BuildProcessTracker::none(),
          m_own_workers.emplace(1).get_scheduler());
    }
    // Only at the start: one removed later is no more than an edit to it.
    if (const auto manifest = m_source.status(k_manifest_name);
        !manifest.has_value() &&
        manifest.error() == std::errc::no_such_file_or_directory) {
      throw std::runtime_error(std::format(
          "no {0} in the source directory, so there is nothing to serve; "
          "create {0} there, or run makebelieve from the directory that has "
          "one",
          k_manifest_name));
    }
    auto rules = read_rules(m_source);
    if (!rules.has_value()) {
      throw std::runtime_error(rules.error());
    }
    set_rules(std::move(*rules));

    // Subscribe only once the outputs are in place, so the callback cannot race
    // the constructor. Restoring from the cache holds the callback off
    // meanwhile, so a change that lands while stamps are compared is applied
    // after - never lost between a stamp and the subscription.
    const std::lock_guard gate_lock(m_gate->mutex);
    m_source_subscription = source.subscribe_to_changes(
        [this, gate = m_gate](const DirectoryTreeDiff& diff) {
          const std::lock_guard lock(gate->mutex);
          if (gate->open) {
            on_source_change(diff);
          }
        });
    if (m_cache != nullptr) {
      // Best-effort: whatever it restored before failing is consistent, and
      // the rest builds afresh. Never thrown out of here, where a callback
      // may already be waiting on the gate to run against this object.
      try {
        restore_from_cache();
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }
    }
  }

  ~Impl() {
    // Close the gate first, so no source notification runs against the state we
    // are about to destroy; then stop observing, cancel the builds under way
    // and wait for each to complete.
    {
      const std::lock_guard lock(m_gate->mutex);
      m_gate->open = false;
    }
    m_source_subscription.reset();
    m_builds.request_stop();
    stdexec::sync_wait(m_builds.join());
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] std::expected<EntryInfo, std::error_code> status(
      const std::filesystem::path& path,
      const OpenContext& context) {
    record_look(path, context, false);
    return m_structure.status(path);
  }

  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path,
      const OpenContext& context) {
    record_look(path, context, true);
    return m_structure.ls(path);
  }

  // Records, against the build whose command @a context says asked, that it
  // looked up - or, if @a listed, listed - @a path through the mount. A
  // request from anything else, such as a command outside any build,
  // records nothing.
  void record_look(const std::filesystem::path& path,
                   const OpenContext& context,
                   bool listed) {
    if (context.requester_pid == 0) {
      return;
    }
    {
      // Nothing to resolve while no build is under way.
      const std::lock_guard lock(m_mutex);
      if (m_reads.empty()) {
        return;
      }
    }
    // Unlocked: it may read /proc.
    const std::optional<std::filesystem::path> reader =
        m_coordinator->resolve(context.requester_pid);
    if (!reader.has_value()) {
      return;
    }
    const std::filesystem::path output = path.lexically_normal();
    const std::lock_guard lock(m_mutex);
    const auto reads = m_reads.find(*reader);
    if (reads == m_reads.end()) {
      return;
    }
    bool& was_listed = reads->second[as_input(output)];
    was_listed = was_listed || listed;
    // What it saw of an output that is not yet what it will be.
    if (output != *reader && m_commands.contains(output) &&
        (m_stale.contains(output) || m_in_flight.contains(output))) {
      m_saw_out_of_date[output].insert(*reader);
    }
  }

  // What a `status` output holds: the manifest's problems, then each build
  // under way, then each output whose last build failed and is not building
  // again, each in path order.
  [[nodiscard]] std::string status_report() {
    const std::lock_guard lock(m_mutex);
    return status_report_locked();
  }

  // As status_report(). @pre m_mutex is held.
  [[nodiscard]] std::string status_report_locked() const {
    std::string report;
    if (!m_manifest_problems.empty()) {
      report += m_manifest_problems;
      report += '\n';
    }
    for (const auto& [output, claims] : m_conflicts) {
      append_conflict(report, output, claims);
    }
    for (const auto& [output, claimed] : m_claimed) {
      // One whose rule went, or became a report's, mid-build is no longer the
      // report's concern.
      const auto rule = m_commands.find(output);
      if (Manifest::is_report(claimed.action) || rule == m_commands.end() ||
          Manifest::is_report(rule->second.action)) {
        continue;
      }
      append_output(report, output);
      report +=
          m_running.contains(output) ? " building: " : " waiting to build: ";
      append_command(report, claimed);
      if (m_failures.contains(output)) {
        report += " (its last build failed)";
      }
      report += '\n';
    }
    for (const auto& [output, failure] : m_failures) {
      if (!m_in_flight.contains(output)) {
        append_failure(report, output, failure,
                       failure.failed_input.has_value() &&
                           is_failing(*failure.failed_input));
      }
    }
    return report;
  }

  // Waits, through the coordinator, for the build of a declared output that
  // brings it up to date - joining the one under way unless the output has
  // changed since it started, otherwise the next, which is started here if the
  // output is idle - and returns that build's error, if it failed. An open by
  // one of our builds' commands is recorded as that build's read, waiting or
  // not.
  [[nodiscard]] exec::task<std::error_code> build_now(
      std::filesystem::path path,
      OpenContext context) {
    const std::filesystem::path output = path.lexically_normal();
    std::optional<std::filesystem::path> requester;
    if (context.requester_pid != 0 && is_declared(output)) {
      // Unlocked: it may read /proc.
      requester = m_coordinator->resolve(context.requester_pid);
      if (requester.has_value()) {
        record_read(*requester, output);
      }
    }
    // Read all the same, so whatever read it rebuilds once it is resolved.
    if (is_conflicting(output)) {
      co_return std::make_error_code(std::errc::io_error);
    }
    const BuildCoordinator::AttemptPtr target =
        bring_up_to_date(output, context.trace_lane);
    if (target == nullptr) {
      co_return std::error_code{};
    }
    co_return co_await m_coordinator->wait_as(target, requester);
  }

  exec::task<std::expected<std::shared_ptr<const OpenedFile>, std::error_code>>
  open(std::filesystem::path path, OpenContext context) {
    if (const auto stop = co_await stdexec::get_stop_token();
        stop.stop_requested()) {
      co_return std::unexpected(
          std::make_error_code(std::errc::operation_canceled));
    }
    if (const auto error = co_await build_now(path, context)) {
      co_return std::unexpected(error);
    }
    // Its build may have succeeded just as the path became a conflict, which
    // left only the placeholder in place.
    if (is_conflicting(path.lexically_normal())) {
      co_return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    co_return co_await m_structure.open(path);
  }

  void materialize_all() {
    std::vector<std::filesystem::path> outputs;
    {
      const std::lock_guard lock(m_mutex);
      m_materialize_all = true;
      for (const auto& [output, command] : m_commands) {
        if (!Manifest::is_report(command.action)) {
          m_materialized.insert(output);
          outputs.push_back(output);
        }
      }
    }
    // Unlocked: a build that completes inline calls finish_build from inside.
    // Nothing traced asked for these, so no arrow leads to them.
    for (const std::filesystem::path& output : outputs) {
      start_build(output, std::nullopt);
    }
  }

  // Passes on every change but the rewrite of a `tracing` output. Reading the
  // trace is what rewrites it, so announcing that would have anything that
  // rereads a file when told it changed - an editor showing it, say - reread
  // the trace forever. So is a `status` output, rewritten for a read; one
  // rewritten because what it shows changed is announced, as
  // status_changed() describes.
  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) {
    std::list<std::function<void(const DirectoryTreeDiff&)>>::iterator ours;
    {
      const std::lock_guard lock(m_subscribers_mutex);
      ours = m_subscribers.emplace(m_subscribers.end(), callback);
    }
    std::optional<Subscription> structure = subscribe_to_structure(callback);
    return Subscription(
        [this, ours, structure = std::move(structure)]() mutable {
          structure.reset();
          const std::lock_guard lock(m_subscribers_mutex);
          m_subscribers.erase(ours);
        });
  }

 private:
  // Tells @a callback of every change to m_structure but the rewrite of a
  // `tracing` or `status` output - the second told of by tell_of_status().
  [[nodiscard]] Subscription subscribe_to_structure(
      const std::function<void(const DirectoryTreeDiff&)>& callback) {
    return m_structure.subscribe_to_changes(
        [this, callback](const DirectoryTreeDiff& diff) {
          if (diff.everything_dirty) {
            callback(diff);
            return;
          }
          DirectoryTreeDiff filtered = diff;
          {
            // Callers hold at most m_layout_mutex, which comes first.
            const std::lock_guard lock(m_mutex);
            std::erase_if(filtered.changed_paths,
                          [&](const std::filesystem::path& path) {
                            const auto it = m_commands.find(path);
                            return it != m_commands.end() &&
                                   Manifest::is_report(it->second.action);
                          });
          }
          // Outside m_mutex, so the callback may read back through us.
          if (!filtered.empty()) {
            callback(filtered);
          }
        });
  }

  // Tells every subscriber that @a outputs, all `status` outputs, changed.
  // @pre No lock of the tree's is held.
  void tell_of_status(std::vector<std::filesystem::path> outputs) {
    if (outputs.empty()) {
      return;
    }
    std::vector<std::function<void(const DirectoryTreeDiff&)>> callbacks;
    {
      const std::lock_guard lock(m_subscribers_mutex);
      callbacks.assign(m_subscribers.begin(), m_subscribers.end());
    }
    DirectoryTreeDiff diff;
    for (std::filesystem::path& output : outputs) {
      diff.changed_paths.insert(std::move(output));
    }
    for (const auto& callback : callbacks) {
      callback(diff);
    }
  }

  // A successful build as it is served.
  struct Built {
    // What serves its content: the file its runner left, opened; the output
    // a copy of it copied, as that was then; or bytes it gave, in memory.
    std::shared_ptr<const OpenedFile> file;

    std::size_t size = 0;

    // The hash of its content, worked out only while there is a cache.
    std::string hash;

    // The file its runner left, still to be moved into the cache or deleted.
    ScratchFile scratch;

    std::vector<Input> inputs;
  };
  using Outcome = std::expected<Built, BuildFailure>;

  // @a result as it is served, from an opened file: the one its runner left,
  // which reads on whatever becomes of it; @a copied, for a copy of an
  // output; or else its bytes, held in memory. Fails as the build does if that
  // file cannot be opened, or hashed for the cache.
  [[nodiscard]] Outcome ingest(BuildResult result,
                               std::shared_ptr<const OpenedFile> copied) const {
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    Built built{.inputs = std::move(result->inputs)};
    const auto fail = [&built](std::error_code error) {
      return std::unexpected(BuildFailure(error, std::move(built.inputs)));
    };
    if (auto* const file =
            std::get_if<std::filesystem::path>(&result->content)) {
      built.scratch = ScratchFile(*file);
      std::expected<std::shared_ptr<const OpenedFile>, std::error_code> opened =
          OpenedFileUtil::open(*file);
      if (!opened.has_value()) {
        return fail(opened.error());
      }
      copied = std::move(*opened);
    }
    if (copied == nullptr) {
      copied = DirectoryTreeUtil::memory_file(
          std::make_shared<const std::string>(
              std::get<std::string>(std::move(result->content))),
          std::chrono::file_clock::time_point());
    }
    const std::expected<FileInfo, std::error_code> info = copied->stat();
    if (!info.has_value()) {
      return fail(info.error());
    }
    built.size = info->size;
    if (m_cache != nullptr) {
      std::expected<std::string, std::error_code> hash =
          BuildCache::hash(*copied);
      if (!hash.has_value()) {
        return fail(hash.error());
      }
      built.hash = std::move(*hash);
    }
    built.file = std::move(copied);
    return built;
  }

  // A successful build to keep in the cache, as finish_build found it.
  struct Keep {
    Command command;
    std::vector<Input> inputs;
    std::chrono::file_clock::time_point mtime;
    std::uint64_t invalidations_at_start = 0;
  };

  // A build claimed under m_mutex, for launch_build to start once it is
  // released.
  struct Launch {
    std::filesystem::path output;
    BuildCoordinator::AttemptPtr attempt;
    Command command;
    std::uint64_t build_id = 0;

    // The trace row whose span asked for the build, which its arrow is drawn
    // from. Without one there is no arrow, since there is no span to bind it
    // to.
    std::optional<TraceLane> origin;
  };

  // The build that brings @a output up to date, as build_now describes, or
  // nullptr if it is up to date already or not declared. A build started here
  // is traced as asked for by @a origin's span.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  BuildCoordinator::AttemptPtr bring_up_to_date(
      const std::filesystem::path& output,
      std::optional<TraceLane> origin) {
    BuildCoordinator::AttemptPtr target;
    std::optional<Launch> launch;
    bool status_output = false;
    {
      const std::lock_guard lock(m_mutex);
      const auto command = m_commands.find(output);
      if (command == m_commands.end()) {
        return nullptr;
      }
      status_output = command->second.action == Manifest::Action::Status;
    }
    // Built by nobody, so never stale - unless its rule changed since, which
    // read_status() finds, and the output is brought up to date as any other.
    if (status_output && read_status(output)) {
      return nullptr;
    }
    {
      const std::lock_guard lock(m_mutex);
      const auto command = m_commands.find(output);
      // Changed back to `status`, even: served as it is.
      if (command == m_commands.end() ||
          command->second.action == Manifest::Action::Status) {
        return nullptr;
      }
      m_materialized.insert(output);

      // A trace is out of date the moment anything else happens, so each open
      // takes a fresh one.
      if (command->second.action == Manifest::Action::Tracing &&
          !m_in_flight.contains(output)) {
        m_stale.insert(output);
      }

      Attempts& attempts = m_attempts[output];
      if (m_in_flight.contains(output)) {
        if (!m_stale.contains(output)) {
          target = attempts.current;
        } else {
          if (attempts.next == nullptr) {
            attempts.next =
                m_coordinator->create_attempt(output, attempts.current);
          }
          target = attempts.next;
        }
      } else if (m_stale.contains(output)) {
        launch = claim_build(output);
        launch->origin = origin;
        target = launch->attempt;
      } else {
        return nullptr;
      }
    }
    // Unlocked: a build that completes inline calls finish_build from inside.
    if (launch.has_value()) {
      launch_build(std::move(*launch));
    }
    return target;
  }

  // The build, as @a self, of a copy of the output @a source, which on success
  // sets @a copied to the version of @a source it copied - shared, not read.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  exec::task<BuildResult> copy_output(
      std::filesystem::path source,
      BuildCoordinator::AttemptPtr self,
      std::optional<TraceLane> lane,
      std::shared_ptr<std::shared_ptr<const OpenedFile>> copied) {
    // Its input even if the copy fails, so the original building retries it -
    // or, for a conflict, resolving it does.
    if (is_conflicting(source)) {
      co_return std::unexpected(BuildFailure(
          std::make_error_code(std::errc::io_error), {as_input(source)}));
    }
    if (const BuildCoordinator::AttemptPtr target =
            bring_up_to_date(source, lane)) {
      if (const std::error_code error = co_await m_coordinator->wait_as(
              target, BuildCoordinator::output_of(self))) {
        co_return std::unexpected(BuildFailure(error, {as_input(source)}));
      }
    }
    std::expected<std::shared_ptr<const OpenedFile>, std::error_code> opened =
        co_await m_structure.open(source);
    if (!opened.has_value()) {
      co_return std::unexpected(
          BuildFailure(opened.error(), {as_input(source)}));
    }
    *copied = std::move(*opened);
    co_return BuildOutput{.inputs = {as_input(source)}};
  }

  // Whether @a output is a path the manifest declares, whether one rule
  // claims it or more than one.
  [[nodiscard]] bool is_declared(const std::filesystem::path& output) {
    const std::lock_guard lock(m_mutex);
    return m_commands.contains(output) || m_conflicts.contains(output);
  }

  // Whether more than one rule claims @a output.
  [[nodiscard]] bool is_conflicting(const std::filesystem::path& output) {
    const std::lock_guard lock(m_mutex);
    return m_conflicts.contains(output);
  }

  // Whether opening @a output fails for now: its last build failed, or more
  // than one rule claims it.
  // @pre m_mutex is held.
  [[nodiscard]] bool is_failing(const std::filesystem::path& output) const {
    return m_failures.contains(output) || m_conflicts.contains(output);
  }

  // Records that the build of @a reader opened @a output, if that build is one
  // of ours under way.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named by the comment
  void record_read(const std::filesystem::path& reader,
                   const std::filesystem::path& output) {
    const std::lock_guard lock(m_mutex);
    if (const auto reads = m_reads.find(reader); reads != m_reads.end()) {
      reads->second.try_emplace(as_input(output), false);
    }
    // It reads the output once up to date, whatever it saw before.
    if (const auto saw = m_saw_out_of_date.find(output);
        saw != m_saw_out_of_date.end()) {
      saw->second.erase(reader);
    }
  }

  // Marks @a output as building - as the attempt an open is already waiting
  // on, if one is - and returns what launch_build needs to start it.
  // @pre m_mutex is held; @a output is declared, stale and not in flight.
  Launch claim_build(const std::filesystem::path& output) {
    Attempts& attempts = m_attempts[output];
    BuildCoordinator::AttemptPtr attempt =
        attempts.next != nullptr ? std::exchange(attempts.next, nullptr)
                                 : m_coordinator->create_attempt(output);
    attempts.current = attempt;
    attempts.invalidations_at_start = m_invalidations[output];
    m_stale.erase(output);
    m_in_flight.insert(output);
    m_claimed.insert_or_assign(output, m_commands.at(output));
    m_reads.try_emplace(output);
    Launch launch{.output = output,
                  .attempt = std::move(attempt),
                  .command = m_commands.at(output)};
    // A `tracing` output is left out of the trace, whose snapshot would
    // otherwise always hold its own unfinished build, and so is a `status`
    // output, which is no build either.
    if (!Manifest::is_report(launch.command.action)) {
      launch.build_id = ++m_last_build_id;
    }
    return launch;
  }

  // Starts @a output's build if it is declared, stale and idle, traced as
  // asked for by @a origin's span.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  void start_build(const std::filesystem::path& output,
                   std::optional<TraceLane> origin) {
    std::optional<Launch> launch;
    {
      const std::lock_guard lock(m_mutex);
      if (!m_stale.contains(output) || m_in_flight.contains(output) ||
          !m_commands.contains(output)) {
        return;
      }
      launch = claim_build(output);
    }
    launch->origin = origin;
    launch_build(std::move(*launch));
  }

  // Recursive with finish_build: a build that completes inline and finds its
  // output went stale meanwhile starts the next one. Each round needs another
  // change to the output's inputs, or an open waiting on the next, so it
  // cannot run away.
  // NOLINTNEXTLINE(misc-no-recursion)
  void launch_build(Launch launch) {
    const std::filesystem::path output = launch.output;
    const std::uint64_t build_id = launch.build_id;
    // As it ran, which the runner takes, for whatever its failure is put down
    // to.
    const Command command = launch.command;

    // Served by the tree itself rather than the runner. A status report is
    // filled in by finish_build, under the lock that orders it with
    // write_status, so it cannot overwrite a newer one.
    if (Manifest::is_report(launch.command.action)) {
      finish_build(
          output, launch.command, launch.attempt, build_id,
          ingest(BuildOutput{.content = launch.command.action ==
                                                Manifest::Action::Tracing
                                            ? trace_snapshot()
                                            : std::string(),
                             .inputs = {}},
                 nullptr));
      return;
    }

    // Waiting its turn, now.
    status_changed();

    std::optional<TraceLane> lane;
    if (Tracer* const tracer = g_tracer) {
      // The arrow leaves whatever asked for this output - an open, a source
      // change that dirtied it, a build copying it - and arrives at the
      // build's own row.
      lane = take_lane(output);
      if (launch.origin.has_value()) {
        tracer->flow_out(*launch.origin, "build", "build", build_id);
      }
      TraceArgs args;
      args.add("action", to_string(launch.command.action));
      args.add("command", launch.command.text);
      tracer->begin(*lane, "build", output, args);
      if (launch.origin.has_value()) {
        tracer->flow_in(*lane, "build", "build", build_id);
      }
    }

    // What a copy of an output copied, once it has.
    auto copied = std::make_shared<std::shared_ptr<const OpenedFile>>();
    std::optional<AttemptSender> build;
    try {
      // A copy of another output is served by the tree too.
      const bool copies_output =
          launch.command.action == Manifest::Action::Copy &&
          launch.command.text.starts_with(k_generated_prefix);
      BuildSender work =
          copies_output
              ? BuildSender(stdexec::write_env(
                    copy_output(
                        launch.command.text.substr(k_generated_prefix.size()),
                        launch.attempt, lane, copied),
                    stdexec::prop{stdexec::get_start_scheduler,
                                  stdexec::inline_scheduler{}}))
              : m_runner(std::move(launch.command));
      // Running once it has a permit, which is when the coordinator starts it.
      BuildSender running =
          stdexec::just() |
          stdexec::let_value([this, output, work = std::move(work)]() mutable {
            {
              const std::lock_guard lock(m_mutex);
              if (m_in_flight.contains(output)) {
                m_running.insert(output);
              }
            }
            status_changed();
            return std::move(work);
          });
      build.emplace(m_coordinator->run(launch.attempt, std::move(running)));
    } catch (...) {
      finish_build(output, command, launch.attempt, build_id,
                   std::unexpected(to_error_code(std::current_exception())));
      return;
    }

    // Started, not awaited, and outside the lock: a build that completes inline
    // re-enters finish_build from within spawn. Every way a build can end is
    // turned into a result, so a waiting open() is always released.
    stdexec::spawn(
        std::move(*build) |
            stdexec::upon_error([](const std::exception_ptr& error) noexcept {
              return BuildResult(
                  std::unexpected(BuildFailure(to_error_code(error))));
            }) |
            stdexec::upon_stopped([]() noexcept {
              return BuildResult(std::unexpected(BuildFailure(
                  std::make_error_code(std::errc::operation_canceled))));
            }) |
            // Recording a result allocates; with nowhere left to report that
            // to, a failure there ends the process, as it would on any thread.
            // Taking in what it built reaches the disk, so happens here,
            // where the build completed, rather than under a lock.
            stdexec::then(
                // NOLINTNEXTLINE(bugprone-exception-escape)
                [this, output, command, attempt = std::move(launch.attempt),
                 build_id, copied](BuildResult result) noexcept {
                  finish_build(output, command, attempt, build_id,
                               ingest(std::move(result), *copied));
                }),
        m_builds.get_token());
  }

  // Takes a row for @a output's build.
  // @pre There is a tracer.
  TraceLane take_lane(const std::filesystem::path& output) {
    const TraceLane lane = m_build_lanes.take(*g_tracer, ProcessInfo::self());
    const std::lock_guard lock(m_lane_mutex);
    m_busy_lanes.insert_or_assign(output, lane);
    return lane;
  }

  // The row @a output's build is being recorded on, if it has one.
  [[nodiscard]] std::optional<TraceLane> lane_of(
      const std::filesystem::path& output) {
    const std::lock_guard lock(m_lane_mutex);
    const auto it = m_busy_lanes.find(output);
    return it == m_busy_lanes.end() ? std::optional<TraceLane>()
                                    : std::optional(it->second);
  }

  // Hands @a output's row back for the next build to take. Called once its
  // span has been closed, so no other build can start one on that row first.
  void free_lane(const std::filesystem::path& output) {
    std::optional<TraceLane> lane;
    {
      const std::lock_guard lock(m_lane_mutex);
      if (const auto it = m_busy_lanes.find(output); it != m_busy_lanes.end()) {
        lane = it->second;
        m_busy_lanes.erase(it);
      }
    }
    if (lane.has_value()) {
      m_build_lanes.give_back(*lane);
    }
  }

  // Records the outcome of @a attempt, a build of @a output by @a ran, then
  // hands it to everything waiting on that attempt. @a build_id is 0 for one
  // left out of the trace.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  void finish_build(const std::filesystem::path& output,
                    const Command& ran,
                    const BuildCoordinator::AttemptPtr& attempt,
                    std::uint64_t build_id,
                    Outcome result) {
    // What to keep in the cache once waiters are released, if anything.
    std::optional<Keep> keep;
    // What saw the output before this build, and is out of date once it is
    // in place.
    std::vector<std::filesystem::path> saw_before;
    if (result.has_value()) {
      const LayoutLock layout(m_layout_mutex, this);
      bool declared = false;
      const auto built_at = std::chrono::file_clock::now();
      {
        const std::lock_guard lock(m_mutex);
        // Its rule may have left the manifest mid-build.
        const auto command = m_commands.find(output);
        declared = command != m_commands.end();
        if (declared) {
          if (command->second.action == Manifest::Action::Status) {
            auto report =
                std::make_shared<const std::string>(status_report_locked());
            result->size = report->size();
            if (m_cache != nullptr) {
              result->hash = BuildCache::hash(*report);
            }
            result->file = DirectoryTreeUtil::memory_file(
                std::move(report), std::chrono::file_clock::time_point());
          }
          add_reads(output, result->inputs);
          if (m_cache != nullptr) {
            // Before any waiter is released, so a build reading this output
            // finds the hash of what it read.
            m_hashes.insert_or_assign(output, result->hash);
            if (!Manifest::is_report(command->second.action)) {
              keep = Keep{.command = command->second,
                          .inputs = result->inputs,
                          .mtime = built_at,
                          .invalidations_at_start =
                              m_attempts[output].invalidations_at_start};
            }
          }
          update_dependents(output, std::move(result->inputs));
        }
      }
      // Outside m_mutex: write_file notifies subscribers, who may read back
      // through us.
      if (declared) {
        if (const std::function<void()> hook = take_before_publish()) {
          hook();
        }
        m_structure.write_file(
            output, result->file,
            FileInfo{.size = result->size, .mtime = built_at});
      }
      // A file the runner left that the cache will not take goes now, the
      // open file serving it reading on, before anything waiting is released.
      if (!keep.has_value()) {
        result->scratch = ScratchFile();
      }
    }

    std::optional<Launch> next;
    BuildCoordinator::AttemptPtr orphaned;
    std::error_code orphaned_error =
        std::make_error_code(std::errc::no_such_file_or_directory);
    std::string failure_report;
    {
      const std::lock_guard lock(m_mutex);
      // A failure that read nothing - one stopped, or that never got to run
      // its command - says nothing of what the output depends on, so keeps
      // the dependencies it had. Never cached: a failure is not kept.
      if (!result.has_value() && m_commands.contains(output)) {
        add_reads(output, result.error().inputs);
        if (!result.error().inputs.empty()) {
          update_dependents(output, result.error().inputs);
        }
      }
      failure_report = record_outcome(output, ran, result);
      // Only now it is in place, and under the lock that ends its being in
      // flight, so that a look at it finds it up to date or is taken here.
      if (result.has_value()) {
        if (auto saw = m_saw_out_of_date.extract(output); !saw.empty()) {
          saw_before.assign(saw.mapped().begin(), saw.mapped().end());
        }
      }
      m_in_flight.erase(output);
      m_running.erase(output);
      m_claimed.erase(output);
      m_reads.erase(output);
      const auto attempts = m_attempts.find(output);
      const bool waited_on =
          attempts != m_attempts.end() && attempts->second.next != nullptr;
      bool rebuild = false;
      if (!m_commands.contains(output)) {
        // Removed mid-build: nothing to record or rebuild, and whatever waits
        // on a next build never gets one.
        if (attempts != m_attempts.end()) {
          orphaned = std::move(attempts->second.next);
          m_attempts.erase(attempts);
        }
        // Or claimed by another rule too, which opening it fails as.
        if (m_conflicts.contains(output)) {
          orphaned_error = std::make_error_code(std::errc::io_error);
        }
      } else {
        // Nothing but a change to its inputs or rule marks an output stale
        // while its build runs, so one stale now changed mid-build: rebuild it
        // if watched, or if an open waits on the next build. A failure is
        // retried the same way, as the change may be its fix - unless it was
        // stopped, as the tree is going. An output whose first build is
        // running has no inputs recorded yet, so a change to them goes unseen.
        const bool changed = m_stale.contains(output);
        const bool stopped =
            !result.has_value() &&
            result.error().error == std::errc::operation_canceled;
        // A failed build stays stale, so a later open or input change retries -
        // unless its rule became a report's mid-build, which nobody builds.
        if (!result.has_value() &&
            !Manifest::is_report(m_commands.at(output).action)) {
          m_stale.insert(output);
        }
        rebuild = waited_on ||
                  (changed && !stopped && m_materialized.contains(output));
      }
      if (rebuild) {
        next = claim_build(output);
      }
    }
    // Before the release, so whoever opened the output can find it reported.
    // Finished, so no longer building, whatever happened.
    status_changed();
    if (!failure_report.empty() && m_on_build_failure) {
      m_on_build_failure(failure_report);
    }

    // Only now the content is in place may a waiting open() be released.
    m_coordinator->complete(
        attempt, result.has_value() ? std::error_code() : result.error().error);
    if (orphaned != nullptr) {
      m_coordinator->complete(orphaned, orphaned_error);
    }

    // After the release, as nothing waiting needs it. A file the runner left
    // that the cache does not take goes with `result`.
    if (keep.has_value()) {
      keep_in_cache(output, *result, *keep);
    }

    const std::optional<TraceLane> lane =
        build_id != 0 ? lane_of(output) : std::nullopt;
    if (lane.has_value()) {
      TraceArgs args;
      args.add("result", result.has_value() ? std::string("ok")
                                            : result.error().error.message());
      g_tracer->end(*lane, "build", output, args);
      // Only now the span is closed may another build take this row.
      free_lane(output);
    }

    // Once nothing is held, as an eager rebuild of one may start here.
    if (!saw_before.empty()) {
      invalidate(std::move(saw_before));
    }

    if (next.has_value()) {
      launch_build(std::move(*next));
    }
  }

  // Appends @a failure, of @a output, as a report gives it: a line saying what
  // failed and why, followed by what it wrote to standard error, indented. If
  // @a input_failing - the failed output it read still is - a copy of that
  // output gets the line alone, naming it, while a command's line names it as
  // well, beside its own reason.
  static void append_failure(std::string& report,
                             const std::filesystem::path& output,
                             const Failure& failure,
                             bool input_failing) {
    append_output(report, output);
    const bool blamed = input_failing && failure.failed_input.has_value();
    if (blamed && failure.copies_failed_input) {
      report += " failed because ";
      append_output(report, *failure.failed_input);
      report += " failed\n";
      return;
    }
    report += " failed (";
    append_reason(report, failure.error);
    if (blamed) {
      report += "; it read ";
      append_output(report, *failure.failed_input);
      report += ", which failed";
    }
    report += "): ";
    append_command(report, failure.command);
    report += '\n';
    append_indented(report, failure.standard_error);
  }

  // Appends that @a output is claimed by more than one rule, as a report gives
  // it: a line naming it, followed by each of @a claims, indented, as
  // `build.makebelieve:LINE: <rule>`, then what a pattern matched and, for a
  // rule that would make a directory of @a output a file, which.
  static void append_conflict(std::string& report,
                              const std::filesystem::path& output,
                              const std::vector<Claim>& claims) {
    append_output(report, output);
    report += " is claimed by more than one rule:\n";
    for (const Claim& claim : claims) {
      report +=
          std::format("    {}:{}: {}", k_manifest_name, claim.line, claim.rule);
      const bool matched = !claim.matched.empty();
      const bool above = claim.command.output != output;
      if (matched || above) {
        report += " (";
        if (matched) {
          report += "matching ";
          report += claim.matched.generic_string();
        }
        if (above) {
          report += matched ? "; it makes " : "it makes ";
          append_output(report, claim.command.output);
          report += " a file";
        }
        report += ')';
      }
      report += '\n';
    }
  }

  // Records how @a output's build by @a ran went, as @a result says: a success
  // clears any failure it had, a failure replaces it - unless the rule changed
  // mid-build, when it says nothing of the rule now. Returns the failure as
  // append_failure() gives it, for the build failure handler - one whose rule
  // changed included, as it did fail - and nothing for a success, for an
  // output whose rule went, or for a build stopped before it could finish,
  // which says nothing of the output.
  // @pre m_mutex is held, and the inputs of a failure hold what it read.
  std::string record_outcome(const std::filesystem::path& output,
                             const Command& ran,
                             const Outcome& result) {
    const auto command = m_commands.find(output);
    const bool current = command != m_commands.end() && command->second == ran;
    if (Manifest::is_report(ran.action) || result.has_value()) {
      if (current) {
        m_failures.erase(output);
      }
      return {};
    }
    if (command == m_commands.end() ||
        result.error().error == std::errc::operation_canceled) {
      return {};
    }

    Failure failure{.command = ran,
                    .error = result.error().error,
                    .standard_error = result.error().standard_error,
                    .failed_input = std::nullopt,
                    .copies_failed_input = false};
    // A copy of an output copies that output; otherwise, every output it read
    // is among its inputs, those it opened through us added already.
    const std::optional<std::filesystem::path> copied =
        ran.action == Manifest::Action::Copy ? output_named_by(ran.text)
                                             : std::nullopt;
    for (const Input& input : result.error().inputs) {
      if (const std::optional<std::filesystem::path> named =
              output_named_by(input.path);
          named.has_value() && *named != output && is_failing(*named)) {
        failure.failed_input = named;
        failure.copies_failed_input = named == copied;
        break;
      }
    }
    std::string report;
    append_failure(report, output, failure, failure.failed_input.has_value());
    if (current) {
      m_failures.insert_or_assign(output, std::move(failure));
    }
    return report;
  }

  // Says that what a `status` output shows may have changed. The first such
  // change since one was last read is written at once, which announces it;
  // after that nothing is, until the next read writes what it shows then. So
  // whoever reads one hears of a change once, however many builds start and
  // finish meanwhile, and decides for themselves when to look again.
  // Best-effort: a status that cannot be written is only out of date, so this
  // never throws into the build that changed it.
  void status_changed() noexcept {
    try {
      // Counted, and announced only if nothing is, without the layout lock,
      // which every build would otherwise queue on.
      {
        const std::lock_guard lock(m_mutex);
        if (m_status_outputs.empty()) {
          return;
        }
        ++m_status_version;
      }
      announce_status();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
  }

  // Writes what the `status` outputs show now, and tells the subscribers, if
  // nothing is announced and they are behind. A subscriber may read them as
  // it is told, which rearms the announcement - and changes may come
  // meanwhile, or while it found nothing to write, not announced as this one
  // was. So it goes round again until either a change is announced and not yet
  // read, or they are up to date.
  // @pre No lock of the tree's is held, as subscribers are told here.
  void announce_status() {
    while (true) {
      {
        const std::lock_guard lock(m_mutex);
        // With no `status` output left - its rule gone since the change was
        // counted - there is nothing to bring up to date.
        if (m_status_outputs.empty() || m_status_announced ||
            m_status_written == m_status_version) {
          return;
        }
        // Before telling, so a read as it is told rearms it.
        m_status_announced = true;
      }
      if (const std::function<void()> hook = take_before_status_write()) {
        hook();
      }
      std::vector<std::filesystem::path> written;
      {
        const LayoutLock layout(m_layout_mutex, this);
        written = write_status();
      }
      if (written.empty()) {
        // Nothing they show changed, so nothing was announced.
        const std::lock_guard lock(m_mutex);
        m_status_announced = false;
        continue;
      }
      tell_of_status(std::move(written));
    }
  }

  // Brings every `status` output up to date for a read of @a output,
  // unannounced - which a reader needs only if a change was announced since
  // the last read - and rearms status_changed(). Returns false, doing nothing,
  // if @a output is no longer a `status` output, its rule having changed since
  // the caller looked.
  //
  // One opened by a subscriber told of a change to some other output - while
  // a layout lock is held, this tree's or another's - is served as it is if
  // this tree's lock is held, so the subscriber never waits on it, and the
  // next read brings it up to date. One told of a change to a `status`
  // output holds no lock, so is served as any other.
  bool read_status(const std::filesystem::path& output) {
    const auto is_status = [&] {
      const auto command = m_commands.find(output);
      return command != m_commands.end() &&
             command->second.action == Manifest::Action::Status;
    };
    std::optional<LayoutLock> layout;
    if (t_layout_holder == nullptr) {
      layout.emplace(m_layout_mutex, this);
    } else if (t_layout_holder != this) {
      layout.emplace(m_layout_mutex, this, std::try_to_lock);
    }
    if (!layout.has_value() || !layout->held()) {
      const std::lock_guard lock(m_mutex);
      return is_status();
    }
    bool behind = false;
    {
      const std::lock_guard lock(m_mutex);
      if (!is_status()) {
        return false;
      }
      m_status_announced = false;
      behind = m_status_written != m_status_version;
    }
    if (behind) {
      // Not announced: this reader is about to read it.
      static_cast<void>(write_status());
    }
    return true;
  }

  // Rewrites every `status` output with what it shows now - which m_structure
  // does not announce - leaving be one that already holds it, and returns
  // those it wrote.
  // @pre m_layout_mutex is held.
  [[nodiscard]] std::vector<std::filesystem::path> write_status() {
    std::vector<std::filesystem::path> outputs;
    std::string report;
    {
      const std::lock_guard lock(m_mutex);
      // Up to date either way, so nothing waits on it.
      m_status_written = m_status_version;
      if (m_status_outputs.empty()) {
        return {};
      }
      report = status_report_locked();
    }
    for (const std::filesystem::path& output : m_status_outputs) {
      if (const std::expected<std::string, std::error_code> current =
              DirectoryTreeUtil::wait_read(
                  DirectoryTreeUtil::read_whole(m_structure, output));
          !current.has_value() || *current != report) {
        outputs.push_back(output);
      }
    }
    if (outputs.empty()) {
      return {};
    }
    note_status_hash(report);
    // Outside m_mutex, as write_file notifies m_structure's subscribers.
    const auto now = std::chrono::file_clock::now();
    for (const std::filesystem::path& output : outputs) {
      m_structure.write_file(output, report, now);
    }
    return outputs;
  }

  // Keeps the hash of @a report as that of every `status` output, for a build
  // that reads one, as finish_build does for other outputs.
  void note_status_hash(const std::string& report) {
    if (m_cache == nullptr) {
      return;
    }
    const std::string hash = BuildCache::hash(report);
    const std::lock_guard lock(m_mutex);
    for (const std::filesystem::path& output : m_status_outputs) {
      m_hashes.insert_or_assign(output, hash);
    }
  }

  // The stamp of @a input, a path with nothing at it, or a directory listing
  // @a entries - or nothing if those could not be had.
  static std::optional<BuildCache::Input> stamp_listing_or_missing(
      const std::filesystem::path& input,
      const std::expected<std::vector<TreeEntry>, std::error_code>& entries) {
    if (entries.has_value()) {
      return BuildCache::Input{.path = input,
                               .kind = BuildCache::Input::Kind::listing,
                               .hash = BuildCache::hash_entries(*entries)};
    }
    if (is_missing(entries.error())) {
      return BuildCache::Input{.path = input,
                               .kind = BuildCache::Input::Kind::missing};
    }
    return std::nullopt;
  }

  // The stamp of a directory at @a input that was only looked at.
  static BuildCache::Input stamp_directory(const std::filesystem::path& input) {
    return BuildCache::Input{.path = input,
                             .kind = BuildCache::Input::Kind::directory};
  }

  // Whether @a stamp is of a directory that was listed.
  static bool stamps_listing(const BuildCache::Input& stamp) {
    return stamp.kind == BuildCache::Input::Kind::listing;
  }

  // The stamp of whatever is at the source path @a path now - a file's mtime
  // and size, standing in for its content; a directory's entries, if
  // @a listed, or else that a directory is there; or that nothing is there -
  // or nothing if it cannot be had.
  [[nodiscard]] std::optional<BuildCache::Input> stamp_source(
      const std::filesystem::path& path,
      bool listed) const {
    const std::expected<EntryInfo, std::error_code> info =
        m_source.status(path);
    if (info.has_value()) {
      if (const auto* const file = std::get_if<FileInfo>(&*info)) {
        return BuildCache::Input{.path = path,
                                 .kind = BuildCache::Input::Kind::file,
                                 .mtime = BuildCache::raw_mtime(file->mtime),
                                 .size = static_cast<std::int64_t>(file->size)};
      }
      return listed ? stamp_listing_or_missing(path, m_source.ls(path))
                    : stamp_directory(path);
    }
    return stamp_listing_or_missing(path, std::unexpected(info.error()));
  }

  // The stamp of whatever is at the output path @a input (`@/` and the path)
  // in m_structure now: a directory's entries, if @a listed, or else that a
  // directory is there; that nothing is there; or, given @a hash - the hash
  // of the output's content, if it has one - a file. Nothing if it is a file
  // with no hash, or if it cannot be had.
  [[nodiscard]] std::optional<BuildCache::Input> stamp_output(
      const std::filesystem::path& input,
      const std::string* hash,
      bool listed) const {
    const std::optional<std::filesystem::path> output = output_named_by(input);
    if (!output.has_value()) {
      return std::nullopt;
    }
    const std::expected<EntryInfo, std::error_code> info =
        m_structure.status(*output);
    if (!info.has_value()) {
      return stamp_listing_or_missing(input, std::unexpected(info.error()));
    }
    if (std::holds_alternative<DirectoryInfo>(*info)) {
      return listed ? stamp_listing_or_missing(input, m_structure.ls(*output))
                    : stamp_directory(input);
    }
    if (hash == nullptr) {
      return std::nullopt;
    }
    return BuildCache::Input{
        .path = input, .kind = BuildCache::Input::Kind::output, .hash = *hash};
  }

  // The output @a input names, if it names one - or a directory of them -
  // rather than a source path: `@` for the root of the outputs, or `@/` and
  // the path.
  static std::optional<std::filesystem::path> output_named_by(
      const std::filesystem::path& input) {
    const std::string text = input.generic_string();
    if (text == "@") {
      return std::filesystem::path();
    }
    if (!text.starts_with(k_generated_prefix)) {
      return std::nullopt;
    }
    return std::filesystem::path(text.substr(k_generated_prefix.size()));
  }

  // Whether nothing has invalidated @a output since the build @a keep describes
  // started.
  // @pre m_mutex is held.
  [[nodiscard]] bool is_current(const std::filesystem::path& output,
                                const Keep& keep) const {
    const auto invalidations = m_invalidations.find(output);
    return (invalidations == m_invalidations.end()
                ? 0
                : invalidations->second) == keep.invalidations_at_start;
  }

  // Keeps @a output's build, @a built, in the cache - handing it the file the
  // runner left, if there is one. If something invalidated it since it
  // started, this leaves the cache alone, as whatever did so drops the
  // output's record itself, and may since have been followed by a newer build
  // whose record this must not touch. If an input it read cannot be stamped,
  // the output is left with no record.
  void keep_in_cache(const std::filesystem::path& output,
                     Built& built,
                     const Keep& keep) {
    std::vector<BuildCache::Input> stamps;
    // The hash of each output it read that has one, as served.
    std::map<std::filesystem::path, std::string> hashes;
    bool keepable = true;
    {
      const std::lock_guard lock(m_mutex);
      if (!is_current(output, keep)) {
        return;
      }
      for (const Input& input : keep.inputs) {
        if (const std::optional<std::filesystem::path> read =
                output_named_by(input.path)) {
          if (const auto hash = m_hashes.find(*read); hash != m_hashes.end()) {
            hashes.emplace(input.path, hash->second);
          }
        }
      }
    }
    // Stamped outside both locks, as a source stamp reaches the disk and an
    // output's reaches m_structure, so that builds finishing together do not
    // queue behind each other's inputs. A change landing after the stamp
    // invalidates the output: either that is seen below, or its record is
    // dropped once this one is in place.
    for (const Input& input : keep.inputs) {
      if (!keepable) {
        break;
      }
      std::optional<BuildCache::Input> stamp;
      if (output_named_by(input.path).has_value()) {
        const auto hash = hashes.find(input.path);
        stamp = stamp_output(input.path,
                             hash == hashes.end() ? nullptr : &hash->second,
                             input.listed);
      } else {
        stamp = stamp_source(input.path, input.listed);
      }
      keepable = stamp.has_value();
      if (keepable) {
        stamps.push_back(std::move(*stamp));
      }
    }

    const std::lock_guard cache_lock(m_cache_mutex);
    {
      const std::lock_guard lock(m_mutex);
      if (!is_current(output, keep)) {
        return;
      }
    }
    // Best-effort: a build the cache cannot keep is just built again next run.
    try {
      if (keepable && !built.scratch.empty()) {
        m_cache->store_file(keep.command, built.scratch.release(), built.hash,
                            keep.mtime, stamps);
      } else if (keepable) {
        // A copy of an output, whose content is in the cache already unless
        // that output was not kept, or is not yet - or bytes a runner gave.
        m_cache->store_content(keep.command, *built.file, built.hash,
                               keep.mtime, stamps);
      } else {
        m_cache->forget(std::span(&output, 1));
      }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
  }

  // Drops the cache's records of @a outputs.
  void forget_in_cache(const std::vector<std::filesystem::path>& outputs) {
    if (m_cache == nullptr || outputs.empty()) {
      return;
    }
    const std::lock_guard cache_lock(m_cache_mutex);
    // Best-effort, as for keep_in_cache. A record left behind holds stamps
    // that no longer match, so the next run still finds it stale.
    try {
      m_cache->forget(outputs);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
  }

  // What restore_from_cache works out as it goes.
  struct Restoring {
    // Every record, by the output it is of.
    std::map<std::filesystem::path, const BuildCache::Record*> records;
    // The rules in force.
    std::map<std::filesystem::path, Command> commands;
    // Whether each output's record holds, worked out once each.
    enum class Check : std::uint8_t { checking, holds, stale };
    std::map<std::filesystem::path, Check> checked;
    // The content of every output whose record holds, opened, and its size.
    std::map<std::filesystem::path,
             std::pair<std::shared_ptr<const OpenedFile>, std::size_t>>
        contents;
  };

  // Whether @a output's record in @a restoring holds: its rule is unchanged,
  // every source file it read is stamped as it is now, every output it read
  // holds too with the content it read, and its own content is intact. An
  // output on a cycle finds its own record does not.
  // NOLINTNEXTLINE(misc-no-recursion): through what it read; `checked` ends it
  bool record_holds(const std::filesystem::path& output,
                    Restoring& restoring) const {
    using Check = Restoring::Check;
    if (const auto it = restoring.checked.find(output);
        it != restoring.checked.end()) {
      return it->second == Check::holds;
    }
    restoring.checked.emplace(output, Check::checking);
    const bool holds = check_record(output, restoring);
    restoring.checked.insert_or_assign(output,
                                       holds ? Check::holds : Check::stale);
    return holds;
  }

  // Works out, once, what record_holds says of @a output, keeping its
  // content in @a restoring if so.
  // NOLINTNEXTLINE(misc-no-recursion): see record_holds
  bool check_record(const std::filesystem::path& output,
                    Restoring& restoring) const {
    const auto record = restoring.records.find(output);
    const auto command = restoring.commands.find(output);
    if (record == restoring.records.end() ||
        command == restoring.commands.end() ||
        Manifest::is_report(command->second.action) ||
        record->second->command != command->second) {
      return false;
    }
    for (const BuildCache::Input& input : record->second->inputs) {
      if (const std::optional<std::filesystem::path> read =
              output_named_by(input.path)) {
        // An output holds, with the content that was read; a directory of
        // outputs, or nothing, is as it was.
        if (input.kind == BuildCache::Input::Kind::output) {
          if (!record_holds(*read, restoring) ||
              restoring.records.at(*read)->hash != input.hash) {
            return false;
          }
        } else if (stamp_output(input.path, nullptr, stamps_listing(input)) !=
                   input) {
          return false;
        }
      } else if (stamp_source(input.path, stamps_listing(input)) != input) {
        return false;
      }
    }
    std::expected<std::shared_ptr<const OpenedFile>, std::error_code> file =
        m_cache->open(record->second->hash);
    if (!file.has_value()) {
      return false;
    }
    const std::expected<FileInfo, std::error_code> info = (*file)->stat();
    if (!info.has_value()) {
      return false;
    }
    restoring.contents.emplace(output, std::pair(std::move(*file), info->size));
    return true;
  }

  // Serves every output whose cached build still holds as built, and drops
  // every other record. Called from the constructor, with source changes held
  // off.
  void restore_from_cache() {
    std::vector<BuildCache::Record> records;
    try {
      records = m_cache->records();
    } catch (...) {
      return;  // as if empty: everything builds afresh
    }
    Restoring restoring;
    for (const BuildCache::Record& record : records) {
      restoring.records.emplace(record.command.output, &record);
    }
    {
      const std::lock_guard lock(m_mutex);
      restoring.commands = m_commands;
    }

    std::vector<std::filesystem::path> dropped;
    {
      const LayoutLock layout(m_layout_mutex, this);
      for (const BuildCache::Record& record : records) {
        const std::filesystem::path& output = record.command.output;
        if (!record_holds(output, restoring)) {
          dropped.push_back(output);
          continue;
        }
        // Its placeholder is already in place, so its directories are too.
        // Served from the cache's own file, which is never rewritten.
        const auto& [file, size] = restoring.contents.at(output);
        m_structure.write_file(output, file,
                               FileInfo{.size = size, .mtime = record.mtime});
        const std::lock_guard lock(m_mutex);
        m_stale.erase(output);
        m_hashes.insert_or_assign(output, record.hash);
        std::vector<Input> inputs;
        inputs.reserve(record.inputs.size());
        for (const BuildCache::Input& input : record.inputs) {
          inputs.emplace_back(input.path, stamps_listing(input));
        }
        update_dependents(output, std::move(inputs));
      }
    }
    try {
      m_cache->forget(dropped);
      m_cache->collect_garbage();
    } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort, as above
    }
  }

  // Refreshes the reverse index for @a output to @a inputs: drops the output
  // from its previous inputs' dependent sets and adds it to the new ones.
  // @pre m_mutex is held.
  void update_dependents(const std::filesystem::path& output,
                         std::vector<Input> inputs) {
    const auto previous = m_dependencies.find(output);
    if (previous != m_dependencies.end()) {
      for (const Input& input : previous->second) {
        const auto it = m_dependents.find(FileSystemUtil::path_key(input.path));
        if (it != m_dependents.end()) {
          it->second.erase(output);
          if (it->second.empty()) {
            m_dependents.erase(it);
          }
        }
      }
    }
    for (const Input& input : inputs) {
      bool& listed = m_dependents[FileSystemUtil::path_key(input.path)][output];
      listed = listed || input.listed;
    }
    m_dependencies.insert_or_assign(output, std::move(inputs));
  }

  // Adds to @a into every output that depends on what is at @a path: on its
  // entries alone, if @a entries_only, as a change to them leaves the rest
  // of what is there alone - so only one that listed it - or else on
  // anything about it. @pre m_mutex is held.
  void add_dependents(std::vector<std::filesystem::path>& into,
                      const std::filesystem::path& path,
                      bool entries_only) const {
    if (const auto readers = m_dependents.find(FileSystemUtil::path_key(path));
        readers != m_dependents.end()) {
      for (const auto& [reader, listed] : readers->second) {
        if (listed || !entries_only) {
          into.push_back(reader);
        }
      }
    }
  }

  // Adds to @a into every output that depends on something @a diff changes,
  // whose paths are as inputs name them: source paths, or `@/` and an output.
  // @pre m_mutex is held.
  void add_affected(std::vector<std::filesystem::path>& into,
                    const DirectoryTreeDiff& diff) const {
    for (const std::filesystem::path& path : diff.changed_paths) {
      add_dependents(into, path, false);
    }
    for (const std::filesystem::path& path : diff.changed_listings) {
      add_dependents(into, path, true);
    }
    for (const std::filesystem::path& path : diff.changed_subtrees) {
      const std::filesystem::path key = FileSystemUtil::path_key(path);
      for (auto it = m_dependents.lower_bound(key);
           it != m_dependents.end() && is_within(it->first, key); ++it) {
        for (const auto& [reader, listed] : it->second) {
          into.push_back(reader);
        }
      }
    }
  }

  // Whether @a diff changes which files a pattern could match: the entries
  // of a directory it matches in or one on the way there, or that directory
  // coming or going, alone or beneath something that did.
  [[nodiscard]] bool changes_pattern_directory(
      const DirectoryTreeDiff& diff) const {
    const auto named = [&](const std::filesystem::path& path) {
      return m_pattern_directories.contains(
          FileSystemUtil::path_key(path.lexically_normal()));
    };
    const auto holds_one = [&](const std::filesystem::path& path) {
      const std::filesystem::path key =
          FileSystemUtil::path_key(path.lexically_normal());
      const auto it = m_pattern_directories.lower_bound(key);
      return it != m_pattern_directories.end() && is_within(*it, key);
    };
    // Anything beneath a directory a pattern lists recursively.
    const auto beneath_one = [&](const std::filesystem::path& path) {
      const std::filesystem::path key =
          FileSystemUtil::path_key(path.lexically_normal());
      return std::ranges::any_of(m_recursive_directories,
                                 [&](const std::filesystem::path& top) {
                                   return is_within(key, top);
                                 });
    };
    return std::ranges::any_of(diff.changed_listings, named) ||
           std::ranges::any_of(diff.changed_subtrees, holds_one) ||
           std::ranges::any_of(diff.changed_listings, beneath_one) ||
           std::ranges::any_of(diff.changed_subtrees, beneath_one);
  }

  // Adds to @a inputs, those @a output's build read, the outputs it opened,
  // looked up or listed through us, which tracing does not see - whether it
  // built or failed. @pre m_mutex is held, and the build's reads are still
  // in m_reads.
  void add_reads(const std::filesystem::path& output,
                 std::vector<Input>& inputs) const {
    if (const auto reads = m_reads.find(output); reads != m_reads.end()) {
      for (const auto& [path, listed] : reads->second) {
        inputs.emplace_back(path, listed);
      }
      merge_inputs(inputs);
    }
  }

  // Rereads the manifest and applies its rules, or reports why it cannot and
  // keeps serving the current ones.
  void reload_manifest() {
    auto rules = read_rules(m_source);
    bool problems_changed = false;
    {
      const std::lock_guard lock(m_mutex);
      std::string problems = rules.has_value() ? std::string() : rules.error();
      problems_changed = problems != m_manifest_problems;
      m_manifest_problems = std::move(problems);
    }
    if (problems_changed) {
      status_changed();
    }
    if (!rules.has_value()) {
      if (m_on_manifest_error) {
        m_on_manifest_error(rules.error());
      }
      return;
    }
    set_rules(std::move(*rules));
  }

  // Takes @a rules as the manifest's and declares the outputs they stand for.
  void set_rules(std::vector<Manifest::Rule> rules) {
    m_rules = std::move(rules);
    m_pattern_directories.clear();
    m_recursive_directories.clear();
    for (const Manifest::Rule& rule : m_rules) {
      if (!rule.is_pattern() || rule.pattern->over_outputs) {
        continue;
      }
      if (rule.pattern->recursive) {
        m_recursive_directories.insert(
            FileSystemUtil::path_key(rule.pattern->directory));
      }
      // Its ancestors too, down to the root: the directory itself may be the
      // thing that comes or goes.
      std::filesystem::path directory = rule.pattern->directory;
      while (m_pattern_directories.insert(FileSystemUtil::path_key(directory))
                 .second &&
             !directory.empty()) {
        directory = directory.parent_path();
      }
    }
    apply_rules(expand_rules(m_source, m_rules));
  }

  // Brings the declared outputs in line with @a commands: a new rule's output
  // appears unbuilt, a removed rule's output disappears, and an output whose
  // command changed goes stale (rebuilt eagerly if opened). Outputs whose rule
  // is unchanged keep their built content, unless they read one that was
  // added, removed or changed.
  //
  // A path that more than one rule claims is shown, with its placeholder, but
  // has no command: it is removed as an output, if it was one, and added again
  // once only one rule claims it.
  void apply_rules(Expansion expansion) {
    const std::map<std::filesystem::path, Command>& commands =
        expansion.commands;
    std::vector<std::filesystem::path> removed;
    std::vector<std::filesystem::path> added;
    std::vector<std::filesystem::path> changed;
    // Paths that more than one rule claims now, but did not before; and those
    // that more than one did, but none does now.
    std::vector<std::filesystem::path> now_conflicting;
    std::vector<std::filesystem::path> unclaimed;
    // Each conflict that is new, or claimed by different rules than before,
    // as append_conflict() gives it, for the failure handler.
    std::vector<std::string> conflict_reports;
    // Outputs that were not `status` outputs before, but are now.
    std::vector<std::filesystem::path> now_status;
    // Directories of outputs that may come or go with the outputs that do,
    // mapped to whether each was there before; then only those that did.
    std::map<std::filesystem::path, bool> directories;
    // What a `status` output that is new shows from the start.
    std::string report;
    {
      const LayoutLock layout(m_layout_mutex, this);
      {
        const std::lock_guard lock(m_mutex);
        for (const auto& [output, command] : m_commands) {
          if (!commands.contains(output)) {
            removed.push_back(output);
          }
        }
        for (const auto& [output, command] : commands) {
          const auto it = m_commands.find(output);
          if (it == m_commands.end()) {
            added.push_back(output);
            // Nobody builds a `status` output, so it is never stale.
            if (command.action == Manifest::Action::Status) {
              now_status.push_back(output);
            } else {
              m_stale.insert(output);
            }
            if (m_materialize_all && !Manifest::is_report(command.action)) {
              m_materialized.insert(output);
              // Invalidated below, which builds it as it is now materialized.
              changed.push_back(output);
            }
          } else if (it->second != command) {
            // A failure of what it used to run says nothing of what it runs
            // now; one that is watched rebuilds at once, showing as building.
            m_failures.erase(output);
            if (command.action == Manifest::Action::Status) {
              // Built by nobody: it holds the report from now on.
              m_stale.erase(output);
              m_materialized.erase(output);
              now_status.push_back(output);
            } else {
              // A report's output was never treated as opened.
              if (m_materialize_all && !Manifest::is_report(command.action)) {
                m_materialized.insert(output);
              }
              changed.push_back(output);
            }
          }
        }
        for (const std::filesystem::path& output : removed) {
          m_stale.erase(output);
          m_materialized.erase(output);
          m_hashes.erase(output);
          m_failures.erase(output);
          ++m_invalidations[output];
          // A build under way keeps its attempts until it finishes, which
          // settles whatever waits on them.
          if (!m_in_flight.contains(output)) {
            m_attempts.erase(output);
          }
          update_dependents(output, {});
          m_dependencies.erase(output);
        }
        for (const auto& [output, claims] : expansion.conflicts) {
          const auto before = m_conflicts.find(output);
          if (before == m_conflicts.end()) {
            now_conflicting.push_back(output);
          }
          if (before == m_conflicts.end() || before->second != claims) {
            append_conflict(conflict_reports.emplace_back(), output, claims);
          }
        }
        for (const auto& [output, claims] : m_conflicts) {
          if (!commands.contains(output) &&
              !expansion.conflicts.contains(output)) {
            unclaimed.push_back(output);
          }
        }
        m_commands = commands;
        m_conflicts = std::move(expansion.conflicts);
        m_status_outputs.clear();
        for (const auto& [output, command] : m_commands) {
          if (command.action == Manifest::Action::Status) {
            m_status_outputs.push_back(output);
          }
        }
        if (!now_status.empty()) {
          report = status_report_locked();
          if (m_cache != nullptr) {
            const std::string hash = BuildCache::hash(report);
            for (const std::filesystem::path& output : m_status_outputs) {
              m_hashes.insert_or_assign(output, hash);
            }
          }
        }
      }

      for (const std::vector<std::filesystem::path>* outputs :
           {&removed, &added, &now_conflicting, &unclaimed}) {
        for (const std::filesystem::path& output : *outputs) {
          for (std::filesystem::path directory = output.parent_path();
               !directory.empty(); directory = directory.parent_path()) {
            directories.try_emplace(directory,
                                    m_structure.status(directory).has_value());
          }
        }
      }

      // Removals first, so a new output can take the place of a directory that
      // only removed outputs occupied. A `status` output holds what it shows
      // from the start, never a placeholder. A conflict keeps its place, but
      // not what it was built as.
      for (const std::filesystem::path& output : removed) {
        if (!m_conflicts.contains(output)) {
          remove_with_empty_parents(m_structure, output);
        }
      }
      for (const std::filesystem::path& output : unclaimed) {
        remove_with_empty_parents(m_structure, output);
      }
      for (const std::filesystem::path& output : added) {
        ensure_parent_directories(m_structure, output);
        m_structure.write_file(output, std::ranges::contains(now_status, output)
                                           ? std::string_view(report)
                                           : k_placeholder_content);
      }
      for (const std::filesystem::path& output : now_conflicting) {
        ensure_parent_directories(m_structure, output);
        m_structure.write_file(output, k_placeholder_content);
      }
      for (const std::filesystem::path& output : now_status) {
        if (!std::ranges::contains(added, output)) {
          m_structure.write_file(output, report);
        }
      }
      std::erase_if(directories, [&](const auto& directory) {
        return m_structure.status(directory.first).has_value() ==
               directory.second;
      });
    }

    // Whatever read an output that came or went - or became a conflict, or
    // stopped being one - or became a `status` output, read something else
    // now; as did whatever looked up a directory of outputs that came or
    // went, or listed the directory such a one came or went in.
    {
      DirectoryTreeDiff outputs_changed;
      const auto came_or_went = [&](const std::filesystem::path& path) {
        outputs_changed.changed_subtrees.insert(as_input(path));
        outputs_changed.changed_listings.insert(as_input(path.parent_path()));
      };
      for (const std::vector<std::filesystem::path>* outputs :
           {&removed, &added, &now_conflicting, &unclaimed}) {
        std::ranges::for_each(*outputs, came_or_went);
      }
      for (const std::filesystem::path& output : now_status) {
        outputs_changed.changed_paths.insert(as_input(output));
      }
      for (const auto& [directory, existed] : directories) {
        came_or_went(directory);
      }
      const std::lock_guard lock(m_mutex);
      add_affected(changed, outputs_changed);
    }

    forget_in_cache(removed);

    // One that was there before, and holds the report now, changed.
    std::erase_if(now_status, [&](const std::filesystem::path& output) {
      return std::ranges::contains(added, output);
    });
    tell_of_status(std::move(now_status));

    // A rule removed or changed takes its failure with it, and one under way
    // shows differently, or not at all. Nothing is announced if nothing shows
    // differently.
    status_changed();
    if (m_on_build_failure) {
      for (const std::string& conflict : conflict_reports) {
        m_on_build_failure(conflict);
      }
    }

    // Outside the layout lock, since an eager rebuild writes its result.
    invalidate(std::move(changed));
  }

  // Reloads the manifest if it may have changed, or matches its patterns
  // again if the files they match may have, then collects the outputs
  // depending on whatever changed and invalidates each. Invalidation runs
  // outside the lock, since it may start an eager rebuild.
  void on_source_change(const DirectoryTreeDiff& diff) {
    MB_TRACE_SCOPE("build", "source change", "changed",
                   diff.changed_paths.size() + diff.changed_listings.size() +
                       diff.changed_subtrees.size(),
                   "everything_dirty", diff.everything_dirty);
    if (diff.everything_dirty ||
        diff.changes(std::filesystem::path(k_manifest_name))) {
      reload_manifest();
    } else if (changes_pattern_directory(diff)) {
      apply_rules(expand_rules(m_source, m_rules));
    }

    std::vector<std::filesystem::path> affected;
    {
      const std::lock_guard lock(m_mutex);
      if (diff.everything_dirty) {
        // Which inputs changed is unknown, so every output might be stale.
        affected.reserve(m_commands.size());
        for (const auto& [output, command] : m_commands) {
          affected.push_back(output);
        }
      } else {
        add_affected(affected, diff);
      }
    }
    invalidate(std::move(affected));
  }

  // Marks @a outputs stale along with everything that read them, transitively,
  // then starts the eager rebuilds - only once all are marked, so nothing
  // downstream is served old content meanwhile.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  void invalidate(std::vector<std::filesystem::path> outputs) {
    std::vector<std::filesystem::path> eager;
    // Outputs that read each other must not loop.
    std::set<std::filesystem::path> seen;
    {
      const std::lock_guard lock(m_mutex);
      while (!outputs.empty()) {
        const std::filesystem::path output = std::move(outputs.back());
        outputs.pop_back();
        const auto command = m_commands.find(output);
        if (command == m_commands.end() || !seen.insert(output).second ||
            command->second.action == Manifest::Action::Status) {
          continue;
        }
        m_stale.insert(output);
        ++m_invalidations[output];
        // Eager only if opened and idle; an in-flight build re-runs if stale.
        if (m_materialized.contains(output) && !m_in_flight.contains(output)) {
          eager.push_back(output);
        }
        add_dependents(outputs, as_input(output), false);
      }
    }
    // Before any rebuild starts, so a record of the stale content never
    // outlives it.
    forget_in_cache({seen.begin(), seen.end()});

    // Called only from within on_source_change, whose span is what the
    // rebuilds' arrows leave.
    const std::optional<TraceLane> origin =
        g_tracer != nullptr ? std::optional(g_tracer->here()) : std::nullopt;
    for (const std::filesystem::path& output : eager) {
      start_build(output, origin);
    }
  }
};

BuildDirectoryTree::BuildDirectoryTree(const DirectoryTree& source,
                                       CommandRunner runner,
                                       ManifestErrorHandler on_manifest_error,
                                       BuildCache* cache,
                                       BuildFailureHandler on_build_failure)
    : m_impl(std::make_unique<Impl>(source,
                                    std::move(runner),
                                    nullptr,
                                    std::move(on_manifest_error),
                                    cache,
                                    std::move(on_build_failure))) {}

BuildDirectoryTree::BuildDirectoryTree(const DirectoryTree& source,
                                       CommandRunner runner,
                                       BuildCoordinator& coordinator,
                                       ManifestErrorHandler on_manifest_error,
                                       BuildCache* cache,
                                       BuildFailureHandler on_build_failure)
    : m_impl(std::make_unique<Impl>(source,
                                    std::move(runner),
                                    &coordinator,
                                    std::move(on_manifest_error),
                                    cache,
                                    std::move(on_build_failure))) {}

BuildDirectoryTree::~BuildDirectoryTree() = default;

void BuildDirectoryTreeTestUtil::before_next_status_write(
    std::function<void()> hook) {
  const std::lock_guard lock(g_status_write_hook_mutex);
  g_before_status_write = std::move(hook);
}

void BuildDirectoryTreeTestUtil::before_next_publish(
    std::function<void()> hook) {
  const std::lock_guard lock(g_status_write_hook_mutex);
  g_before_publish = std::move(hook);
}

std::expected<EntryInfo, std::error_code> BuildDirectoryTree::status(
    const std::filesystem::path& path,
    const OpenContext& context) const {
  return m_impl->status(path, context);
}

std::expected<std::vector<TreeEntry>, std::error_code> BuildDirectoryTree::ls(
    const std::filesystem::path& path,
    const OpenContext& context) const {
  return m_impl->ls(path, context);
}

void BuildDirectoryTree::materialize_all() const {
  m_impl->materialize_all();
}

std::string BuildDirectoryTree::status_report() const {
  return m_impl->status_report();
}

OpenSender BuildDirectoryTree::open(const std::filesystem::path& path,
                                    const OpenContext& context) const {
  // Give the task a start scheduler before type erasure; inline leaves
  // continuations on whichever thread completes them.
  return stdexec::write_env(
      m_impl->open(path, context),
      stdexec::prop{stdexec::get_start_scheduler, stdexec::inline_scheduler{}});
}

Subscription BuildDirectoryTree::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  return m_impl->subscribe_to_changes(callback);
}

}  // namespace makebelieve
