// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "buildcoordinator.hpp"
#include "buildpermits.hpp"
#include "directorytreeutil.hpp"
#include "filetask.hpp"
#include "inmemorydirectorytree.hpp"
#include "manifest.hpp"
#include "processinfo.hpp"
#include "tracer.hpp"

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

// Reads the whole of @a path out of @a tree, or the error that stopped it.
// read() promises only "up to size bytes" per call, so this loops until a
// short read signals end of file.
std::expected<std::string, std::error_code> read_all(
    const DirectoryTree& tree,
    const std::filesystem::path& path) {
  const std::expected<EntryInfo, std::error_code> info = tree.status(path);
  if (!info.has_value()) {
    return std::unexpected(info.error());
  }
  if (!std::holds_alternative<FileInfo>(*info)) {
    return std::unexpected(std::make_error_code(std::errc::is_a_directory));
  }

  constexpr std::size_t k_chunk = std::size_t{64} * 1024;
  std::string content;
  while (true) {
    const std::expected<std::string, std::error_code> chunk =
        DirectoryTreeUtil::read(tree, path, static_cast<Offset>(content.size()),
                                k_chunk);
    if (!chunk.has_value()) {
      return std::unexpected(chunk.error());
    }
    content += *chunk;
    if (chunk->size() < k_chunk) {
      return content;
    }
  }
}

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
  }
  return "unknown";
}

// The content of a `tracing` output: the trace so far, or an empty one when
// nothing is being traced.
std::string trace_snapshot() {
  return g_tracer != nullptr ? g_tracer->snapshot() : std::string("[]\n");
}

// Reads the rules of the manifest in @a source, or describes - one problem per
// line - why the manifest could not be read or has lines it cannot accept. No
// manifest at all means no rules.
std::expected<std::vector<Manifest::Rule>, std::string> read_rules(
    const DirectoryTree& source) {
  const std::expected<std::string, std::error_code> manifest =
      read_all(source, k_manifest_name);
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

// Maps each output @a rules declare to its command, a wildcard rule declaring
// one for every file in @a source its pattern matches right now.
std::map<std::filesystem::path, Command> expand_rules(
    const DirectoryTree& source,
    const std::vector<Manifest::Rule>& rules) {
  std::map<std::filesystem::path, Command> commands;
  const auto declare = [&](const Manifest::Rule& rule) {
    commands.emplace(rule.output, Command{.output = rule.output,
                                          .action = rule.action,
                                          .text = rule.command});
  };
  for (const Manifest::Rule& rule : rules) {
    if (!rule.is_wildcard()) {
      declare(rule);
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
        declare(*matched);
      }
    }
  }

  // A wildcard over outputs matches those declared so far, which may be what
  // lets another match, so go round until nothing is added. The manifest
  // rejects wildcards that could feed themselves, so this ends.
  for (bool added = true; added;) {
    added = false;
    for (const Manifest::Rule& rule : rules) {
      if (!rule.wildcard_over_outputs) {
        continue;
      }
      std::vector<Manifest::Rule> matches;
      for (const auto& [output, command] : commands) {
        if (output.parent_path() != rule.wildcard_source.parent_path()) {
          continue;
        }
        if (std::optional<Manifest::Rule> matched =
                Manifest::instantiate(rule, output.filename().generic_string());
            matched.has_value() && !commands.contains(matched->output)) {
          matches.push_back(std::move(*matched));
        }
      }
      for (const Manifest::Rule& matched : matches) {
        declare(matched);
        added = true;
      }
    }
  }
  return commands;
}

}  // namespace

class BuildDirectoryTree::Impl {
  // The directory structure and file contents we present. Each declared output
  // starts as a placeholder, replaced by real content once its command
  // succeeds.
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

  // Outputs that need (re)building: every output starts stale, a successful
  // build clears it, an input change marks it stale again.
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
  };
  std::map<std::filesystem::path, Attempts> m_attempts;

  // The outputs each build under way has opened through us, added to its
  // inputs when it finishes. Tracing sees only the source, so not these.
  std::unordered_map<AttemptId, std::set<std::filesystem::path>> m_reads;

  // Outputs opened at least once, which an input change rebuilds eagerly.
  std::set<std::filesystem::path> m_materialized;

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

  CommandRunner m_runner;

  // Used when the tree is given no coordinator: no limit on builds, and no
  // request attributed to any.
  std::optional<exec::static_thread_pool> m_own_workers;
  std::optional<BuildCoordinator> m_own_coordinator;

  // Runs every build and holds every open that waits on one.
  BuildCoordinator* m_coordinator;

  // Told why a changed manifest was rejected; may be empty.
  ManifestErrorHandler m_on_manifest_error;

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
       ManifestErrorHandler on_manifest_error)
      : m_source(source),
        m_runner(std::move(runner)),
        m_coordinator(coordinator),
        m_on_manifest_error(std::move(on_manifest_error)) {
    if (m_coordinator == nullptr) {
      m_coordinator =
          &m_own_coordinator.emplace(unlimited_permits(), Attribution::none(),
                                     m_own_workers.emplace(1).get_scheduler());
    }
    auto rules = read_rules(m_source);
    if (!rules.has_value()) {
      throw std::runtime_error(rules.error());
    }
    set_rules(std::move(*rules));

    // Subscribe only once the outputs are in place, so the callback cannot race
    // the constructor.
    m_source_subscription = source.subscribe_to_changes(
        [this, gate = m_gate](const DirectoryTreeDiff& diff) {
          const std::lock_guard lock(gate->mutex);
          if (gate->open) {
            on_source_change(diff);
          }
        });
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

  [[nodiscard]] ReadSender read(const std::filesystem::path& path,
                                Offset offset,
                                std::size_t size) const {
    return m_structure.read(path, offset, size);
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
    std::optional<AttemptId> requester;
    if (context.requester_pid != 0 && is_declared(output)) {
      // Unlocked: it may read /proc.
      requester = m_coordinator->resolve(context.requester_pid);
      if (requester.has_value()) {
        record_read(*requester, output);
      }
    }
    const BuildCoordinator::AttemptPtr target = bring_up_to_date(output);
    if (target == nullptr) {
      co_return std::error_code{};
    }
    co_return co_await m_coordinator->wait_as(target, requester);
  }

  exec::task<std::expected<FileInfo, std::error_code>> open(
      std::filesystem::path path,
      OpenContext context) {
    if (const auto stop = co_await stdexec::get_stop_token();
        stop.stop_requested()) {
      co_return std::unexpected(
          std::make_error_code(std::errc::operation_canceled));
    }
    if (const auto error = co_await build_now(path, context)) {
      co_return std::unexpected(error);
    }
    co_return co_await DirectoryTreeUtil::open_by_status(m_structure, path);
  }

  // Passes on every change but the rewrite of a `tracing` output. Reading the
  // trace is what rewrites it, so announcing that would have anything that
  // rereads a file when told it changed - an editor showing it, say - reread
  // the trace forever.
  [[nodiscard]] Subscription subscribe_to_changes(
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
                   it->second.action == Manifest::Action::Tracing &&
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

 private:
  // A build claimed under m_mutex, for launch_build to start once it is
  // released.
  struct Launch {
    std::filesystem::path output;
    BuildCoordinator::AttemptPtr attempt;
    Command command;
    std::uint64_t build_id = 0;
  };

  // The build that brings @a output up to date, as build_now describes, or
  // nullptr if it is up to date already or not declared.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  BuildCoordinator::AttemptPtr bring_up_to_date(
      const std::filesystem::path& output) {
    BuildCoordinator::AttemptPtr target;
    std::optional<Launch> launch;
    {
      const std::lock_guard lock(m_mutex);
      const auto command = m_commands.find(output);
      if (command == m_commands.end()) {
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
            attempts.next = m_coordinator->create_attempt(attempts.current);
          }
          target = attempts.next;
        }
      } else if (m_stale.contains(output)) {
        launch = claim_build(output);
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
                                      BuildCoordinator::AttemptPtr self) {
    if (const BuildCoordinator::AttemptPtr target = bring_up_to_date(source)) {
      if (const std::error_code error = co_await m_coordinator->wait_as(
              target, BuildCoordinator::id_of(self))) {
        co_return std::unexpected(error);
      }
    }
    std::expected<std::string, std::error_code> bytes =
        read_all(m_structure, source);
    if (!bytes.has_value()) {
      co_return std::unexpected(bytes.error());
    }
    co_return BuildOutput{.bytes = std::move(*bytes),
                          .inputs = {as_input(source)}};
  }

  [[nodiscard]] bool is_declared(const std::filesystem::path& output) {
    const std::lock_guard lock(m_mutex);
    return m_commands.contains(output);
  }

  // Records that @a attempt opened @a output, if it is one of ours under way.
  void record_read(AttemptId attempt, const std::filesystem::path& output) {
    const std::lock_guard lock(m_mutex);
    if (const auto reads = m_reads.find(attempt); reads != m_reads.end()) {
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
                                 : m_coordinator->create_attempt();
    attempts.current = attempt;
    m_stale.erase(output);
    m_in_flight.insert(output);
    m_reads.try_emplace(BuildCoordinator::id_of(attempt));
    Launch launch{.output = output,
                  .attempt = std::move(attempt),
                  .command = m_commands.at(output)};
    // A `tracing` output is left out of the trace, whose snapshot would
    // otherwise always hold its own unfinished build.
    if (launch.command.action != Manifest::Action::Tracing) {
      launch.build_id = ++m_last_build_id;
    }
    return launch;
  }

  // Starts @a output's build if it is declared, stale and idle.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  void start_build(const std::filesystem::path& output) {
    std::optional<Launch> launch;
    {
      const std::lock_guard lock(m_mutex);
      if (!m_stale.contains(output) || m_in_flight.contains(output) ||
          !m_commands.contains(output)) {
        return;
      }
      launch = claim_build(output);
    }
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

    // Served by the tree itself rather than the runner.
    if (launch.command.action == Manifest::Action::Tracing) {
      finish_build(output, launch.attempt, build_id,
                   BuildOutput{.bytes = trace_snapshot(), .inputs = {}});
      return;
    }

    if (Tracer* const tracer = g_tracer) {
      // The arrow leaves whatever we are inside - the open that asked for this
      // output, or the source change that dirtied it - and arrives at the
      // build's own row.
      const TraceLane lane = take_lane(output);
      tracer->flow_out("build", "build", build_id);
      TraceArgs args;
      args.add("action", to_string(launch.command.action));
      args.add("command", launch.command.text);
      tracer->begin(lane, "build", output, args);
      tracer->flow_in(lane, "build", "build", build_id);
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
                        launch.attempt),
                    stdexec::prop{stdexec::get_start_scheduler,
                                  stdexec::inline_scheduler{}}))
              : m_runner(std::move(launch.command));
      build.emplace(m_coordinator->run(launch.attempt, std::move(work)));
    } catch (...) {
      finish_build(output, launch.attempt, build_id,
                   std::unexpected(to_error_code(std::current_exception())));
      return;
    }

    // Started, not awaited, and outside the lock: a build that completes inline
    // re-enters finish_build from within spawn. Every way a build can end is
    // turned into a result, so a waiting open() is always released.
    stdexec::spawn(
        std::move(*build) |
            stdexec::upon_error([](const std::exception_ptr& error) noexcept {
              return BuildResult(std::unexpected(to_error_code(error)));
            }) |
            stdexec::upon_stopped([]() noexcept {
              return BuildResult(std::unexpected(
                  std::make_error_code(std::errc::operation_canceled)));
            }) |
            // Recording a result allocates; with nowhere left to report that
            // to, a failure there ends the process, as it would on any thread.
            stdexec::then(
                // NOLINTNEXTLINE(bugprone-exception-escape)
                [this, output, attempt = std::move(launch.attempt),
                 build_id](BuildResult result) noexcept {
                  finish_build(output, attempt, build_id, std::move(result));
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

  // Records the outcome of @a attempt, a build of @a output, then hands it to
  // everything waiting on that attempt. @a build_id is 0 for one left out of
  // the trace.
  // NOLINTNEXTLINE(misc-no-recursion): see launch_build.
  void finish_build(const std::filesystem::path& output,
                    const BuildCoordinator::AttemptPtr& attempt,
                    std::uint64_t build_id,
                    BuildResult result) {
    if (result.has_value()) {
      const std::lock_guard layout(m_layout_mutex);
      bool declared = false;
      {
        const std::lock_guard lock(m_mutex);
        // Its rule may have left the manifest mid-build.
        declared = m_commands.contains(output);
        if (declared) {
          if (const auto reads = m_reads.find(BuildCoordinator::id_of(attempt));
              reads != m_reads.end()) {
            result->inputs.insert(result->inputs.end(), reads->second.begin(),
                                  reads->second.end());
          }
          update_dependents(output, std::move(result->inputs));
        }
      }
      // Outside m_mutex: write_file notifies subscribers, who may read back
      // through us.
      if (declared) {
        m_structure.write_file(output, std::move(result->bytes));
      }
    }

    std::optional<Launch> next;
    BuildCoordinator::AttemptPtr orphaned;
    {
      const std::lock_guard lock(m_mutex);
      m_in_flight.erase(output);
      m_reads.erase(BuildCoordinator::id_of(attempt));
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
      } else if (result.has_value()) {
        // An input that changed mid-build left it stale again; rebuild if
        // watched, or if an open waits on the next build.
        rebuild = waited_on ||
                  (m_stale.contains(output) && m_materialized.contains(output));
      } else {
        // A failed build stays stale, so a later open or input change retries;
        // an open already waiting on the next build gets it now.
        m_stale.insert(output);
        rebuild = waited_on;
      }
      if (rebuild) {
        next = claim_build(output);
      }
    }
    // Only now the content is in place may a waiting open() be released.
    m_coordinator->complete(
        attempt, result.has_value() ? std::error_code() : result.error());
    if (orphaned != nullptr) {
      m_coordinator->complete(
          orphaned, std::make_error_code(std::errc::no_such_file_or_directory));
    }

    const std::optional<TraceLane> lane =
        build_id != 0 ? lane_of(output) : std::nullopt;
    if (lane.has_value()) {
      TraceArgs args;
      args.add("result", result.has_value() ? std::string("ok")
                                            : result.error().message());
      g_tracer->end(*lane, "build", output, args);
      // Only now the span is closed may another build take this row.
      free_lane(output);
    }

    if (next.has_value()) {
      launch_build(std::move(*next));
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

  // Rereads the manifest and applies its rules, or reports why it cannot and
  // keeps serving the current ones.
  void reload_manifest() {
    auto rules = read_rules(m_source);
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
  void apply_rules(const std::map<std::filesystem::path, Command>& commands) {
    std::vector<std::filesystem::path> removed;
    std::vector<std::filesystem::path> added;
    std::vector<std::filesystem::path> changed;
    {
      const std::lock_guard layout(m_layout_mutex);
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
            m_stale.insert(output);
          } else if (it->second != command) {
            changed.push_back(output);
          }
        }
        for (const std::filesystem::path& output : removed) {
          m_stale.erase(output);
          m_materialized.erase(output);
          // A build under way keeps its attempts until it finishes, which
          // settles whatever waits on them.
          if (!m_in_flight.contains(output)) {
            m_attempts.erase(output);
          }
          update_dependents(output, {});
          m_dependencies.erase(output);
        }
        m_commands = commands;
      }

      // Removals first, so a new output can take the place of a directory that
      // only removed outputs occupied.
      for (const std::filesystem::path& output : removed) {
        remove_with_empty_parents(m_structure, output);
      }
      for (const std::filesystem::path& output : added) {
        ensure_parent_directories(m_structure, output);
        m_structure.write_file(output, k_placeholder_content);
      }
    }

    // Whatever read an output that came or went read something else now.
    {
      const std::lock_guard lock(m_mutex);
      for (const std::vector<std::filesystem::path>* outputs :
           {&removed, &added}) {
        for (const std::filesystem::path& output : *outputs) {
          if (const auto readers = m_dependents.find(as_input(output));
              readers != m_dependents.end()) {
            changed.insert(changed.end(), readers->second.begin(),
                           readers->second.end());
          }
        }
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
    {
      const std::lock_guard lock(m_mutex);
      // Outputs that read each other must not loop.
      std::set<std::filesystem::path> seen;
      while (!outputs.empty()) {
        const std::filesystem::path output = std::move(outputs.back());
        outputs.pop_back();
        if (!m_commands.contains(output) || !seen.insert(output).second) {
          continue;
        }
        m_stale.insert(output);
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
    for (const std::filesystem::path& output : eager) {
      start_build(output);
    }
  }
};

BuildDirectoryTree::BuildDirectoryTree(const DirectoryTree& source,
                                       CommandRunner runner,
                                       ManifestErrorHandler on_manifest_error)
    : m_impl(std::make_unique<Impl>(source,
                                    std::move(runner),
                                    nullptr,
                                    std::move(on_manifest_error))) {}

BuildDirectoryTree::BuildDirectoryTree(const DirectoryTree& source,
                                       CommandRunner runner,
                                       BuildCoordinator& coordinator,
                                       ManifestErrorHandler on_manifest_error)
    : m_impl(std::make_unique<Impl>(source,
                                    std::move(runner),
                                    &coordinator,
                                    std::move(on_manifest_error))) {}

BuildDirectoryTree::~BuildDirectoryTree() = default;

std::expected<EntryInfo, std::error_code> BuildDirectoryTree::status(
    const std::filesystem::path& path) const {
  return m_impl->status(path);
}

std::expected<std::vector<TreeEntry>, std::error_code> BuildDirectoryTree::ls(
    const std::filesystem::path& path) const {
  return m_impl->ls(path);
}

OpenSender BuildDirectoryTree::open(const std::filesystem::path& path,
                                    const OpenContext& context) const {
  return file_task(m_impl->open(path, context));
}

ReadSender BuildDirectoryTree::read(const std::filesystem::path& path,
                                    Offset offset,
                                    std::size_t size) const {
  return m_impl->read(path, offset, size);
}

Subscription BuildDirectoryTree::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  return m_impl->subscribe_to_changes(callback);
}

}  // namespace makebelieve
