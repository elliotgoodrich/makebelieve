// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "inmemorydirectorytree.hpp"
#include "manifest.hpp"
#include "processutil.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace makebelieve {

namespace {

// The default entry point.
constexpr std::string_view k_manifest_name = "build.makebelieve";

// The placeholder in a command that is replaced with the output's path.
constexpr std::string_view k_out_placeholder = "%out";

// The content an output holds before it has ever been built: a single null
// byte. Viewing a named char rather than a string literal keeps the length and
// the storage in step (a `string_view("", 1)` reads as out-of-bounds).
constexpr char k_null_byte = '\0';
constexpr std::string_view k_placeholder_content(&k_null_byte, 1);

// Removes a directory and everything beneath it on destruction, so a command's
// scratch space does not outlive the build it was created for.
class TempDirectory {
  std::filesystem::path m_path;

 public:
  explicit TempDirectory(std::filesystem::path path)
      : m_path(std::move(path)) {}

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  TempDirectory(TempDirectory&&) = delete;
  TempDirectory& operator=(TempDirectory&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
};

std::filesystem::path make_temp_directory() {
  std::random_device device;
  std::uniform_int_distribution<unsigned> digit(0, 0xffffffU);
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  while (true) {
    const std::filesystem::path candidate =
        base / ("makebelieve-build-" + std::to_string(digit(device)));
    std::error_code ec;
    if (std::filesystem::create_directory(candidate, ec)) {
      return candidate;
    }
  }
}

// Replaces every `%out` in @a command with @a out_path, quoted so paths
// containing spaces survive the shell.
std::string substitute_out(std::string_view command,
                           const std::filesystem::path& out_path) {
  const std::string replacement = "\"" + out_path.string() + "\"";
  std::string result;
  std::size_t pos = 0;
  while (true) {
    const std::size_t found = command.find(k_out_placeholder, pos);
    if (found == std::string_view::npos) {
      result += command.substr(pos);
      return result;
    }
    result += command.substr(pos, found - pos);
    result += replacement;
    pos = found + k_out_placeholder.size();
  }
}

// Reads the whole of @a path, or the error that stopped it. ifstream does not
// say why it would not open, so the filesystem is asked instead.
std::expected<std::string, std::error_code> read_file(
    const std::filesystem::path& path) {
  const std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    std::error_code ec;
    const std::filesystem::file_status status =
        std::filesystem::status(path, ec);
    if (ec) {
      return std::unexpected(ec);
    }
    if (!std::filesystem::exists(status)) {
      return std::unexpected(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (std::filesystem::is_directory(status)) {
      return std::unexpected(std::make_error_code(std::errc::is_a_directory));
    }
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

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

// Recasts absolute traced input paths into paths relative to @a root, dropping
// any that fall outside it.
std::vector<std::filesystem::path> relativize(
    const std::vector<std::filesystem::path>& inputs,
    const std::filesystem::path& root) {
  // relative() is lexical, so it only cancels root against an input whose
  // leading components match exactly. Traced inputs are canonicalised and can
  // differ in case from root as given, so canonicalise root to match.
  std::error_code ec;
  std::filesystem::path canonical_root =
      std::filesystem::weakly_canonical(root, ec);
  if (ec) {
    canonical_root = root;
  }

  std::vector<std::filesystem::path> result;
  result.reserve(inputs.size());
  for (const std::filesystem::path& input : inputs) {
    std::error_code relative_ec;
    const std::filesystem::path relative =
        std::filesystem::relative(input, canonical_root, relative_ec);
    if (relative_ec || relative.empty()) {
      continue;
    }
    const std::filesystem::path normal = relative.lexically_normal();
    if (normal.begin() != normal.end() && *normal.begin() == "..") {
      continue;  // escapes the root
    }
    result.push_back(normal);
  }
  return result;
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

// One output's action and its argument, as the runner receives it.
using Command = BuildDirectoryTree::Command;

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
    commands.emplace(rule.output,
                     Command{.action = rule.action, .text = rule.command});
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

  // Requested on destruction to tell in-flight runners their results are no
  // longer wanted.
  std::stop_source m_stop;

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
    // are about to destroy; then stop observing and cancel in-flight runners.
    {
      const std::lock_guard lock(m_gate->mutex);
      m_gate->open = false;
    }
    m_source_subscription.reset();
    m_stop.request_stop();
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
    if (!m_commands.contains(output)) {
      return {};
    }
    m_materialized.insert(output);

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
      // Unlocked: a synchronous runner calls finish_build from inside.
      lock.unlock();
      start_build(output);
      lock.lock();
    }
  }

  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) const {
    return m_structure.subscribe_to_changes(callback);
  }

 private:
  void start_build(const std::filesystem::path& output) {
    Command command;
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
    }

    // Outside the lock: a synchronous runner re-enters finish_build from within
    // this call.
    m_runner(command, m_stop.get_token(),
             // A synchronous completion allocates and lets that escape open().
             // NOLINTNEXTLINE(bugprone-exception-escape)
             [this, output](BuildResult result) {
               finish_build(output, std::move(result));
             });
  }

  void finish_build(const std::filesystem::path& output, BuildResult result) {
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

BuildDirectoryTree::CommandRunner BuildDirectoryTree::shell_runner(
    std::filesystem::path working_directory) {
  return [working_directory = std::move(working_directory)](
             const Command& command, const std::stop_token& stop,
             BuildComplete on_done) {
    // A `copy` rule names a file rather than a command: nothing is run, the
    // output is that file's bytes, and the file itself is the build's one
    // input - so a copy tracks its source even on a platform where command
    // tracing is unavailable. The parser has already held the path to one
    // inside the working directory.
    if (command.action == Manifest::Action::Copy) {
      const std::filesystem::path source = command.text;
      std::expected<std::string, std::error_code> bytes =
          read_file(working_directory / source);
      if (bytes.has_value()) {
        on_done(BuildOutput{.bytes = std::move(*bytes), .inputs = {source}});
      } else {
        on_done(std::unexpected(bytes.error()));
      }
      return;
    }

    const bool capture = command.action == Manifest::Action::Capture;

    // A `run` command gets a private file to write its output to, with that
    // path substituted in for %out. Its build output is whatever it left in
    // that file - not ProcessUtil's captured stdout, which a `> %out` rule
    // leaves empty - so the scratch directory is kept alive (through the
    // shared_ptr the completion captures) until we have read it back. A
    // `capture` command has no output file: its stdout is the output.
    const std::shared_ptr<TempDirectory> scratch =
        capture ? nullptr
                : std::make_shared<TempDirectory>(make_temp_directory());
    const std::filesystem::path out_path =
        capture ? std::filesystem::path{} : scratch->path() / "out";

    ProcessUtil::run(
        working_directory,
        capture ? command.text : substitute_out(command.text, out_path), stop,
        // Reading the output allocates; a synchronous ProcessUtil
        // lets that propagate back out to the triggering read().
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [scratch, out_path, capture, working_directory,
         on_done = std::move(on_done)](ProcessUtil::Result result) mutable {
          if (result.has_value()) {
            on_done(BuildOutput{
                .bytes = capture ? std::move(result->standard_output)
                                 : read_file(out_path).value_or(std::string{}),
                .inputs = relativize(result->inputs, working_directory)});
          } else {
            on_done(std::unexpected(result.error()));
          }
        });
  };
}

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
