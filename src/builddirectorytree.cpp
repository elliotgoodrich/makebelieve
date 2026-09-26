// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "inmemorydirectorytree.hpp"
#include "manifest.hpp"
#include "processutil.hpp"
#include "tracer.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <condition_variable>
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
        tree.read(path, static_cast<Offset>(content.size()), k_chunk);
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

// The error a build that completed with @a error is recorded as.
std::error_code to_error_code(const std::exception_ptr& error) noexcept {
  try {
    std::rethrow_exception(error);
  } catch (const std::system_error& failure) {
    return failure.code();
  } catch (const std::bad_alloc&) {
    return std::make_error_code(std::errc::not_enough_memory);
  } catch (...) {
    return std::make_error_code(std::errc::io_error);
  }
}

// The content of a `tracing` output: the trace so far, or an empty one when
// nothing is being traced.
std::string trace_snapshot() {
  return g_tracer != nullptr ? g_tracer->snapshot() : std::string("[]\n");
}

// Reads the rules of the manifest in @a source, mapping each output to its
// command, or describes - one problem per line - why the manifest could not be
// read or has lines it cannot accept. No manifest at all means no rules.
std::expected<std::map<std::filesystem::path, Command>, std::string> read_rules(
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

  std::map<std::filesystem::path, Command> commands;
  for (const Manifest::Rule& rule : parsed.rules()) {
    commands.emplace(rule.output, Command{.output = rule.output,
                                          .action = rule.action,
                                          .text = rule.command});
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

  // Every declared output mapped to the command that (re)builds it, following
  // the manifest as it changes.
  std::map<std::filesystem::path, Command> m_commands;

  // Outputs that need (re)building: every output starts stale, a successful
  // build clears it, an input change marks it stale again.
  std::set<std::filesystem::path> m_stale;

  // Outputs whose build has started and is awaiting completion, so a second
  // open or a coincident input change does not launch it again.
  std::set<std::filesystem::path> m_in_flight;

  // Signalled when a build finishes, waking a waiting open().
  std::condition_variable m_settled;

  // Builds finished per output, and the last one's error if it failed.
  std::map<std::filesystem::path, std::uint64_t> m_finished;
  std::map<std::filesystem::path, std::error_code> m_failed;

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
       ManifestErrorHandler on_manifest_error)
      : m_source(source),
        m_runner(std::move(runner)),
        m_on_manifest_error(std::move(on_manifest_error)) {
    const auto commands = read_rules(m_source);
    if (!commands.has_value()) {
      throw std::runtime_error(commands.error());
    }
    apply_rules(*commands);

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

  [[nodiscard]] std::expected<std::string, std::error_code> read(
      const std::filesystem::path& path,
      Offset offset,
      std::size_t size) const {
    return m_structure.read(path, offset, size);
  }

  // Builds or waits until a declared output is neither stale nor building.
  // Returns the error of a build that failed meanwhile rather than retrying.
  [[nodiscard]] std::error_code build_now(const std::filesystem::path& path) {
    const std::filesystem::path output = path.lexically_normal();
    std::unique_lock lock(m_mutex);
    const auto command = m_commands.find(output);
    if (command == m_commands.end()) {
      return {};
    }
    m_materialized.insert(output);

    // A trace is out of date the moment anything else happens, so each open
    // takes a fresh one.
    if (command->second.action == Manifest::Action::Tracing &&
        !m_in_flight.contains(output)) {
      m_stale.insert(output);
    }

    const std::uint64_t finished_before = m_finished[output];
    while (true) {
      if (m_in_flight.contains(output)) {
        m_settled.wait(lock);
        continue;
      }
      if (!m_stale.contains(output)) {
        return {};
      }
      // Stale and idle: report a failure since we started, else build.
      if (m_finished[output] != finished_before) {
        if (const auto it = m_failed.find(output); it != m_failed.end()) {
          return it->second;
        }
      }
      // Unlocked: a build that completes inline calls finish_build from inside.
      lock.unlock();
      start_build(output);
      lock.lock();
    }
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
  // Recursive with finish_build: a build that completes inline and finds its
  // output went stale meanwhile starts the next one. Each round needs another
  // change to the output's inputs, so it cannot run away.
  // NOLINTNEXTLINE(misc-no-recursion)
  void start_build(const std::filesystem::path& output) {
    Command command;
    std::uint64_t build_id = 0;
    {
      const std::lock_guard lock(m_mutex);
      if (!m_stale.contains(output) || m_in_flight.contains(output)) {
        return;
      }
      const auto it = m_commands.find(output);
      if (it == m_commands.end()) {
        return;
      }
      m_stale.erase(output);
      m_in_flight.insert(output);
      command = it->second;
      // A `tracing` output is left out of the trace, whose snapshot would
      // otherwise always hold its own unfinished build.
      if (command.action != Manifest::Action::Tracing) {
        build_id = ++m_last_build_id;
      }
    }

    // Served by the tree itself rather than the runner.
    if (command.action == Manifest::Action::Tracing) {
      finish_build(output, build_id,
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
      args.add("action", to_string(command.action));
      args.add("command", command.text);
      tracer->begin(lane, "build", output, args);
      tracer->flow_in(lane, "build", "build", build_id);
    }

    std::optional<BuildSender> build;
    try {
      build.emplace(m_runner(std::move(command)));
    } catch (...) {
      finish_build(output, build_id,
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
                [this, output, build_id](BuildResult result) noexcept {
                  finish_build(output, build_id, std::move(result));
                }),
        m_builds.get_token());
  }

  // Takes a row for @a output's build.
  // @pre There is a tracer.
  TraceLane take_lane(const std::filesystem::path& output) {
    const TraceLane lane = m_build_lanes.take(*g_tracer, ProcessUtil::self());
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

  // Records the outcome of a build. @a build_id is 0 for one left out of the
  // trace.
  // NOLINTNEXTLINE(misc-no-recursion): see start_build.
  void finish_build(const std::filesystem::path& output,
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
          update_dependents(output, std::move(result->inputs));
        }
      }
      // Outside m_mutex: write_file notifies subscribers, who may read back
      // through us.
      if (declared) {
        m_structure.write_file(output, std::move(result->bytes));
      }
    }

    bool rebuild_again = false;
    {
      const std::lock_guard lock(m_mutex);
      // Only now the content is in place may a waiting open() be released.
      m_in_flight.erase(output);
      ++m_finished[output];
      if (!m_commands.contains(output)) {
        // Removed mid-build: nothing to record or rebuild.
      } else if (result.has_value()) {
        m_failed.erase(output);
        // An input that changed mid-build left it stale again; rebuild if
        // watched.
        rebuild_again =
            m_stale.contains(output) && m_materialized.contains(output);
      } else {
        // A failed build stays stale, so a later open or input change retries.
        m_failed.insert_or_assign(output, result.error());
        m_stale.insert(output);
      }
    }
    m_settled.notify_all();

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

    if (rebuild_again) {
      start_build(output);
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
    const auto commands = read_rules(m_source);
    if (!commands.has_value()) {
      if (m_on_manifest_error) {
        m_on_manifest_error(commands.error());
      }
      return;
    }
    apply_rules(*commands);
  }

  // Brings the declared outputs in line with @a commands: a new rule's output
  // appears unbuilt, a removed rule's output disappears, and an output whose
  // command changed goes stale (rebuilt eagerly if opened). Outputs whose rule
  // is unchanged keep their built content.
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
          m_failed.erase(output);
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

    // Outside the layout lock, since an eager rebuild writes its result.
    for (const std::filesystem::path& output : changed) {
      invalidate(output);
    }
  }

  // Reloads the manifest if it may have changed, then collects the outputs
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
    for (const std::filesystem::path& output : affected) {
      invalidate(output);
    }
  }

  void invalidate(const std::filesystem::path& output) {
    bool eager = false;
    {
      const std::lock_guard lock(m_mutex);
      if (!m_commands.contains(output)) {
        return;
      }
      m_stale.insert(output);
      // Eager only if opened and idle; an in-flight build re-runs if stale.
      eager = m_materialized.contains(output) && !m_in_flight.contains(output);
    }
    if (eager) {
      start_build(output);
    }
  }
};

BuildDirectoryTree::BuildDirectoryTree(const DirectoryTree& source,
                                       CommandRunner runner,
                                       ManifestErrorHandler on_manifest_error)
    : m_impl(std::make_unique<Impl>(source,
                                    std::move(runner),
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

std::expected<FileInfo, std::error_code> BuildDirectoryTree::open(
    const std::filesystem::path& path) const {
  if (const std::error_code error = m_impl->build_now(path)) {
    return std::unexpected(error);
  }
  return DirectoryTree::open(path);  // status() now reflects the build
}

std::expected<std::string, std::error_code> BuildDirectoryTree::read(
    const std::filesystem::path& path,
    Offset offset,
    std::size_t size) const {
  return m_impl->read(path, offset, size);
}

Subscription BuildDirectoryTree::subscribe_to_changes(
    const std::function<void(const DirectoryTreeDiff&)>& callback) const {
  return m_impl->subscribe_to_changes(callback);
}

}  // namespace makebelieve
