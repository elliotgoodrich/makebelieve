// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "buildcache.hpp"
#include "buildcoordinator.hpp"
#include "buildpermits.hpp"
#include "directorytreeutil.hpp"
#include "inmemorydirectorytree.hpp"
#include "manifest.hpp"
#include "processinfo.hpp"
#include "scopeguard.hpp"
#include "shellrunner.hpp"
#include "tracer.hpp"

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <chrono>
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

// What BuildDirectoryTreeTestUtil::before_next_status_write() was last given,
// and not yet taken.
std::mutex g_status_write_hook_mutex;
std::function<void()> g_before_status_write;

// Takes the hook to call before the next status announcement writes, if any.
std::function<void()> take_before_status_write() {
  const std::lock_guard lock(g_status_write_hook_mutex);
  return std::exchange(g_before_status_write, nullptr);
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
  return std::filesystem::path("@") / output;
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

  // The source file - or `@/` and the output - a wildcard matched to claim
  // the path. Empty for a simple rule.
  std::filesystem::path matched;

  [[nodiscard]] friend bool operator==(const Claim&, const Claim&) = default;
};

// What a manifest's rules declare, given the files their wildcards match.
struct Expansion {
  // Each path exactly one rule claims, mapped to the command that builds it.
  std::map<std::filesystem::path, Command> commands;

  // Each path more than one rule claims, mapped to every claim on it in line
  // order. Nothing builds it, and opening it fails.
  std::map<std::filesystem::path, std::vector<Claim>> conflicts;
};

// @a rule as its manifest line spells it.
std::string to_string(const Manifest::Rule& rule) {
  std::string text(k_generated_prefix);
  text += rule.output.generic_string();
  text += " = ";
  text += to_string(rule.action);
  if (!rule.command.empty()) {
    text += ' ';
    text += rule.command;
  }
  return text;
}

// Finds every path @a rules claim, a wildcard rule claiming one for every file
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
              .rule = to_string(rule),
              .matched = std::move(matched)});
  };
  for (const Manifest::Rule& rule : rules) {
    if (!rule.is_wildcard()) {
      claim(rule, rule, {});
      continue;
    }
    if (rule.wildcard_over_outputs) {
      continue;  // below, once there are outputs to match
    }
    // A directory that is missing or cannot be listed matches nothing.
    const auto entries = source.ls(rule.wildcard_source.parent_path());
    if (!entries.has_value()) {
      continue;
    }
    for (const TreeEntry& entry : *entries) {
      if (!std::holds_alternative<FileInfo>(entry.info)) {
        continue;
      }
      if (const std::optional<Manifest::Rule> matched =
              Manifest::instantiate(rule, entry.name.generic_string())) {
        claim(rule, *matched, rule.wildcard_source.parent_path() / entry.name);
      }
    }
  }

  // A wildcard over outputs matches every path claimed so far, which may be
  // what lets another match, so go round until nothing is added - matching
  // each path once per rule. The manifest rejects wildcards that could feed
  // themselves, so this ends.
  std::set<std::pair<std::size_t, std::filesystem::path>> tried;
  for (bool added = true; added;) {
    added = false;
    for (std::size_t index = 0; index < rules.size(); ++index) {
      const Manifest::Rule& rule = rules[index];
      if (!rule.wildcard_over_outputs) {
        continue;
      }
      std::vector<std::pair<Manifest::Rule, std::filesystem::path>> matches;
      for (const auto& [output, others] : claims) {
        if (output.parent_path() != rule.wildcard_source.parent_path() ||
            !tried.emplace(index, output).second) {
          continue;
        }
        if (std::optional<Manifest::Rule> matched = Manifest::instantiate(
                rule, output.filename().generic_string())) {
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
  // start.
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
  // of children decides which files their wildcards match. Touched only by the
  // constructor and the source-change callback, so unguarded.
  std::vector<Manifest::Rule> m_rules;
  std::unordered_set<std::filesystem::path> m_wildcard_directories;

  // Every declared output mapped to the command that (re)builds it, following
  // the manifest and the files its wildcards match as they change.
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

  // The outputs each build under way has opened through us, by the output
  // it builds, added to its inputs when it finishes. Tracing sees only the
  // source, so not these.
  std::unordered_map<std::filesystem::path, std::set<std::filesystem::path>>
      m_reads;

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
  std::map<std::filesystem::path, std::vector<std::filesystem::path>>
      m_dependencies;

  // The reverse of m_dependencies: each input mapped to the outputs that read
  // it, matched against a source change to find what to rebuild.
  std::map<std::filesystem::path, std::set<std::filesystem::path>> m_dependents;

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
      const std::filesystem::path& path) const {
    return m_structure.status(path);
  }

  [[nodiscard]] std::expected<std::vector<TreeEntry>, std::error_code> ls(
      const std::filesystem::path& path) const {
    return m_structure.ls(path);
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
          // A rewrite rather than an addition or a removal, which changes the
          // parent's list of children too.
          const auto is_trace_rewrite = [&](const std::filesystem::path& path) {
            const auto it = m_commands.find(path);
            return it != m_commands.end() &&
                   Manifest::is_report(it->second.action) &&
                   !std::ranges::contains(diff.child_lists_changed,
                                          path.parent_path());
          };

          DirectoryTreeDiff filtered;
          {
            // Callers hold at most m_layout_mutex, which comes first.
            const std::lock_guard lock(m_mutex);
            if (std::ranges::none_of(diff.entries_changed, is_trace_rewrite)) {
              filtered = diff;
            } else {
              filtered.child_lists_changed = diff.child_lists_changed;
              std::ranges::copy_if(diff.entries_changed,
                                   std::back_inserter(filtered.entries_changed),
                                   std::not_fn(is_trace_rewrite));
            }
          }
          // Outside m_mutex, so the callback may read back through us.
          if (!filtered.entries_changed.empty() ||
              !filtered.child_lists_changed.empty()) {
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
    const DirectoryTreeDiff diff{.entries_changed = std::move(outputs)};
    for (const auto& callback : callbacks) {
      callback(diff);
    }
  }

 public:
 private:
  // A successful build to keep in the cache, as finish_build found it.
  struct Keep {
    Command command;
    std::vector<std::filesystem::path> inputs;
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

  // The build, as @a self, of a copy of the output @a source.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  exec::task<BuildResult> copy_output(std::filesystem::path source,
                                      BuildCoordinator::AttemptPtr self,
                                      std::optional<TraceLane> lane) {
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
    std::expected<std::string, std::error_code> content =
        co_await DirectoryTreeUtil::read_whole(m_structure, source);
    if (!content.has_value()) {
      co_return std::unexpected(
          BuildFailure(content.error(), {as_input(source)}));
    }
    co_return BuildOutput{.bytes = std::move(*content),
                          .inputs = {as_input(source)}};
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
      reads->second.insert(as_input(output));
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
      finish_build(output, launch.command, launch.attempt, build_id,
                   BuildOutput{.bytes = launch.command.action ==
                                                Manifest::Action::Tracing
                                            ? trace_snapshot()
                                            : std::string(),
                               .inputs = {}});
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
                        launch.attempt, lane),
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
            stdexec::then(
                // NOLINTNEXTLINE(bugprone-exception-escape)
                [this, output, command, attempt = std::move(launch.attempt),
                 build_id](BuildResult result) noexcept {
                  finish_build(output, command, attempt, build_id,
                               std::move(result));
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
                    BuildResult result) {
    // What to keep in the cache once waiters are released, if anything.
    std::optional<Keep> keep;
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
            result->bytes = status_report_locked();
          }
          add_reads(output, result->inputs);
          if (m_cache != nullptr) {
            // Before any waiter is released, so a build reading this output
            // finds the hash of what it read.
            m_hashes.insert_or_assign(output, BuildCache::hash(result->bytes));
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
        m_structure.write_file(output, result->bytes, built_at);
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

    // After the release, as nothing waiting needs it.
    if (keep.has_value()) {
      keep_in_cache(output, result->bytes, *keep);
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
  // `build.makebelieve:LINE: <rule>`, then what a wildcard matched and, for a
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
                             const BuildResult& result) {
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
    for (const std::filesystem::path& input : result.error().inputs) {
      if (const std::optional<std::filesystem::path> named =
              output_named_by(input);
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

  // The stamp of the source file or directory @a path as it is now, or
  // nothing if it cannot be had.
  [[nodiscard]] std::optional<BuildCache::Input> stamp_source(
      const std::filesystem::path& path) const {
    const std::expected<EntryInfo, std::error_code> info =
        m_source.status(path);
    if (!info.has_value()) {
      return std::nullopt;
    }
    if (const auto* const file = std::get_if<FileInfo>(&*info)) {
      return BuildCache::Input{.path = path,
                               .mtime = BuildCache::raw_mtime(file->mtime),
                               .size = static_cast<std::int64_t>(file->size),
                               .hash = {}};
    }
    return BuildCache::Input{
        .path = path,
        .mtime = BuildCache::raw_mtime(std::get<DirectoryInfo>(*info).mtime),
        .size = -1,
        .hash = {}};
  }

  // The output @a input names, if it names one rather than a source file.
  static std::optional<std::filesystem::path> output_named_by(
      const std::filesystem::path& input) {
    const std::string text = input.generic_string();
    if (!text.starts_with(k_generated_prefix)) {
      return std::nullopt;
    }
    return std::filesystem::path(text.substr(k_generated_prefix.size()));
  }

  // Keeps @a output's build, @a bytes, in the cache - unless something
  // invalidated it since it started, or an input it read cannot be stamped,
  // in which case the output is left with no record.
  void keep_in_cache(const std::filesystem::path& output,
                     const std::string& bytes,
                     const Keep& keep) {
    const std::lock_guard cache_lock(m_cache_mutex);
    std::vector<BuildCache::Input> stamps;
    bool keepable = true;
    {
      const std::lock_guard lock(m_mutex);
      const auto invalidations = m_invalidations.find(output);
      keepable = (invalidations == m_invalidations.end()
                      ? 0
                      : invalidations->second) == keep.invalidations_at_start;
      for (const std::filesystem::path& input : keep.inputs) {
        if (!keepable) {
          break;
        }
        if (const std::optional<std::filesystem::path> read =
                output_named_by(input)) {
          const auto hash = m_hashes.find(*read);
          keepable = hash != m_hashes.end();
          if (keepable) {
            stamps.push_back(BuildCache::Input{
                .path = input, .mtime = 0, .size = 0, .hash = hash->second});
          }
        }
      }
    }
    // Source files are stamped outside m_mutex, as that reaches the disk. A
    // change landing after the stamp invalidates the output, whose record is
    // then dropped once this one is in place.
    for (const std::filesystem::path& input : keep.inputs) {
      if (!keepable) {
        break;
      }
      if (!output_named_by(input).has_value()) {
        const std::optional<BuildCache::Input> stamp = stamp_source(input);
        keepable = stamp.has_value();
        if (keepable) {
          stamps.push_back(*stamp);
        }
      }
    }
    // Best-effort: a build the cache cannot keep is just built again next run.
    try {
      if (keepable) {
        m_cache->store(keep.command, bytes, keep.mtime, stamps);
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
    // The content of every output whose record holds.
    std::map<std::filesystem::path, std::string> contents;
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
        if (!record_holds(*read, restoring) ||
            restoring.records.at(*read)->hash != input.hash) {
          return false;
        }
      } else if (stamp_source(input.path) != input) {
        return false;
      }
    }
    std::expected<std::string, std::error_code> bytes =
        m_cache->read(record->second->hash);
    if (!bytes.has_value()) {
      return false;
    }
    restoring.contents.emplace(output, std::move(*bytes));
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
        m_structure.write_file(output, restoring.contents.at(output),
                               record.mtime);
        const std::lock_guard lock(m_mutex);
        m_stale.erase(output);
        m_hashes.insert_or_assign(output, record.hash);
        std::vector<std::filesystem::path> inputs;
        inputs.reserve(record.inputs.size());
        for (const BuildCache::Input& input : record.inputs) {
          inputs.push_back(input.path);
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
                         std::vector<std::filesystem::path> inputs) {
    const auto previous = m_dependencies.find(output);
    if (previous != m_dependencies.end()) {
      for (const std::filesystem::path& input : previous->second) {
        const auto it = m_dependents.find(input);
        if (it != m_dependents.end()) {
          it->second.erase(output);
          if (it->second.empty()) {
            m_dependents.erase(it);
          }
        }
      }
    }
    for (const std::filesystem::path& input : inputs) {
      m_dependents[input].insert(output);
    }
    m_dependencies.insert_or_assign(output, std::move(inputs));
  }

  // Adds to @a inputs, those @a output's build read, the outputs it opened
  // through us, which tracing does not see - whether it built or failed.
  // @pre m_mutex is held, and the build's reads are still in m_reads.
  void add_reads(const std::filesystem::path& output,
                 std::vector<std::filesystem::path>& inputs) const {
    if (const auto reads = m_reads.find(output); reads != m_reads.end()) {
      inputs.insert(inputs.end(), reads->second.begin(), reads->second.end());
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
    m_wildcard_directories.clear();
    for (const Manifest::Rule& rule : m_rules) {
      if (!rule.is_wildcard()) {
        continue;
      }
      // Its ancestors too, down to the root: the directory itself may be the
      // thing that comes or goes.
      std::filesystem::path directory = rule.wildcard_source.parent_path();
      while (m_wildcard_directories.insert(directory).second &&
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
    }

    // Whatever read an output that came or went - or became a conflict, or
    // stopped being one - or became a `status` output, read something else
    // now.
    {
      const std::lock_guard lock(m_mutex);
      for (const std::vector<std::filesystem::path>* outputs :
           {&removed, &added, &now_status, &unclaimed}) {
        for (const std::filesystem::path& output : *outputs) {
          if (const auto readers = m_dependents.find(as_input(output));
              readers != m_dependents.end()) {
            changed.insert(changed.end(), readers->second.begin(),
                           readers->second.end());
          }
        }
      }
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

  // Reloads the manifest if it may have changed, or matches its wildcards
  // again if the files they match may have, then collects the outputs
  // depending on whatever changed and invalidates each. Invalidation runs
  // outside the lock, since it may start an eager rebuild.
  void on_source_change(const DirectoryTreeDiff& diff) {
    MB_TRACE_SCOPE("build", "source change", "changed",
                   diff.entries_changed.size(), "everything_dirty",
                   diff.everything_dirty);
    if (diff.everything_dirty ||
        std::ranges::contains(diff.entries_changed,
                              std::filesystem::path(k_manifest_name))) {
      reload_manifest();
    } else if (std::ranges::any_of(diff.child_lists_changed,
                                   [&](const std::filesystem::path& directory) {
                                     return m_wildcard_directories.contains(
                                         directory.lexically_normal());
                                   })) {
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
        for (const std::filesystem::path& changed : diff.entries_changed) {
          const auto it = m_dependents.find(changed);
          if (it != m_dependents.end()) {
            affected.insert(affected.end(), it->second.begin(),
                            it->second.end());
          }
        }
      }
    }
    invalidate(std::move(affected));
  }

  // Marks @a outputs stale along with everything that read them, transitively,
  // then starts the eager rebuilds - only once all are marked, so nothing
  // downstream is served old content meanwhile.
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
        if (const auto readers = m_dependents.find(as_input(output));
            readers != m_dependents.end()) {
          outputs.insert(outputs.end(), readers->second.begin(),
                         readers->second.end());
        }
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

std::expected<EntryInfo, std::error_code> BuildDirectoryTree::status(
    const std::filesystem::path& path) const {
  return m_impl->status(path);
}

std::expected<std::vector<TreeEntry>, std::error_code> BuildDirectoryTree::ls(
    const std::filesystem::path& path) const {
  return m_impl->ls(path);
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
