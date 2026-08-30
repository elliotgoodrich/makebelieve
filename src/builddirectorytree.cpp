// SPDX-License-Identifier: MIT
#include "builddirectorytree.hpp"

#include "inmemorydirectorytree.hpp"
#include "manifest.hpp"
#include "processutil.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
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

// The content an output holds before it has ever been read: a single null
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

std::string read_file(const std::filesystem::path& path) {
  const std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// Reads the whole of @a path out of @a tree, or nullopt when it is not a
// readable file. read() promises only "up to size bytes" per call, so this
// loops until a short read signals end of file.
std::optional<std::string> read_all(const DirectoryTree& tree,
                                    const std::filesystem::path& path) {
  const std::expected<EntryInfo, std::error_code> info = tree.status(path);
  if (!info.has_value() || !std::holds_alternative<FileInfo>(*info)) {
    return std::nullopt;
  }

  constexpr std::size_t k_chunk = std::size_t{64} * 1024;
  std::string content;
  while (true) {
    const std::expected<std::string, std::error_code> chunk =
        tree.read(path, static_cast<Offset>(content.size()), k_chunk);
    if (!chunk.has_value()) {
      return std::nullopt;
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

}  // namespace

class BuildDirectoryTree::Impl {
  // The directory structure and file contents we present. Each declared output
  // starts as a placeholder, replaced by real content once its command
  // succeeds.
  InMemoryDirectoryTree m_structure;

  // Guards all the build bookkeeping below, which reads (on filesystem threads)
  // and source-change notifications (on the source's watcher thread) race over.
  // Never held across a call to the runner or m_structure, so a synchronous
  // runner or a subscriber reading back through us cannot deadlock on it.
  std::mutex m_mutex;

  // Every declared output mapped to the command that (re)builds it, fixed at
  // construction.
  std::map<std::filesystem::path, std::string> m_commands;

  // Outputs that need (re)building: every output starts stale, a successful
  // build clears it, an input change marks it stale again.
  std::set<std::filesystem::path> m_stale;

  // Outputs whose build has started and is awaiting completion, so a second
  // read or a coincident input change does not launch it again.
  std::set<std::filesystem::path> m_in_flight;

  // Outputs read at least once. An input change rebuilds one of these eagerly;
  // an output nobody has read is only marked stale, to build on its next read.
  std::set<std::filesystem::path> m_materialized;

  // The inputs each built output last read. Kept so a rebuild can refresh
  // m_dependents when an output's set of inputs changes.
  std::map<std::filesystem::path, std::vector<std::filesystem::path>>
      m_dependencies;

  // The reverse of m_dependencies: each input mapped to the outputs that read
  // it, matched against a source change to find what to rebuild.
  std::map<std::filesystem::path, std::set<std::filesystem::path>> m_dependents;

  CommandRunner m_runner;

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
  Impl(const DirectoryTree& source, CommandRunner runner)
      : m_runner(std::move(runner)) {
    const std::optional<std::string> manifest =
        read_all(source, k_manifest_name);
    if (manifest.has_value()) {
      const Manifest parsed = Manifest::parse(*manifest);
      for (const Manifest::Rule& rule : parsed.rules()) {
        ensure_parent_directories(m_structure, rule.output);
        m_structure.write_file(rule.output, k_placeholder_content);
        m_commands.insert_or_assign(rule.output, rule.command);
        m_stale.insert(rule.output);
      }
    }

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

  // The first read of a declared output triggers its build and marks it read
  // (so a later input change rebuilds it eagerly); reads before the build
  // completes see the placeholder.
  [[nodiscard]] std::expected<std::string, std::error_code>
  read(const std::filesystem::path& path, Offset offset, std::size_t size) {
    // A zero-size read or negative offset never yields content, so skip the
    // build it would otherwise trigger.
    if (size != 0 && offset >= 0) {
      const std::filesystem::path output = path.lexically_normal();
      mark_materialized(output);
      start_build(output);
    }
    return m_structure.read(path, offset, size);
  }

  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) const {
    return m_structure.subscribe_to_changes(callback);
  }

 private:
  void mark_materialized(const std::filesystem::path& output) {
    const std::lock_guard lock(m_mutex);
    if (m_commands.contains(output)) {
      m_materialized.insert(output);
    }
  }

  void start_build(const std::filesystem::path& output) {
    std::string command;
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
             // A synchronous completion allocates and lets that escape read().
             // NOLINTNEXTLINE(bugprone-exception-escape)
             [this, output](BuildResult result) {
               finish_build(output, std::move(result));
             });
  }

  void finish_build(const std::filesystem::path& output, BuildResult result) {
    bool rebuild_again = false;
    {
      const std::lock_guard lock(m_mutex);
      m_in_flight.erase(output);
      if (!result.has_value()) {
        // A failed build stays stale, so a later read or input change retries.
        m_stale.insert(output);
        return;
      }
      update_dependents(output, std::move(result->inputs));
      // An input that changed mid-build left it stale again; rebuild if
      // watched.
      rebuild_again =
          m_stale.contains(output) && m_materialized.contains(output);
    }

    // Outside the lock: write_file notifies subscribers, who may read back
    // through us.
    m_structure.write_file(output, std::move(result->bytes));

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

  // Collects the outputs depending on whatever changed, then invalidates each.
  // Invalidation runs outside the lock, since it may start an eager rebuild.
  void on_source_change(const DirectoryTreeDiff& diff) {
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
      // Eager only if read and not already building; an in-flight build re-runs
      // on completion when it finds it stale.
      eager = m_materialized.contains(output) && !m_in_flight.contains(output);
    }
    if (eager) {
      start_build(output);
    }
  }
};

BuildDirectoryTree::BuildDirectoryTree(const DirectoryTree& source,
                                       CommandRunner runner)
    : m_impl(std::make_unique<Impl>(source, std::move(runner))) {}

BuildDirectoryTree::~BuildDirectoryTree() = default;

BuildDirectoryTree::CommandRunner BuildDirectoryTree::shell_runner(
    std::filesystem::path working_directory) {
  return [working_directory = std::move(working_directory)](
             const std::string& command, const std::stop_token& stop,
             BuildComplete on_done) {
    // Give the command a private file to write its output to and substitute
    // that path in for %out. The build output is whatever the command left in
    // that file - not ProcessUtil's captured stdout, which a `> %out` rule
    // leaves empty - so the scratch directory is kept alive (through the
    // shared_ptr the completion captures) until we have read it back.
    const auto scratch = std::make_shared<TempDirectory>(make_temp_directory());
    const std::filesystem::path out_path = scratch->path() / "out";

    ProcessUtil::run(
        working_directory, substitute_out(command, out_path), stop,
        // Reading the output allocates; a synchronous ProcessUtil
        // lets that propagate back out to the triggering read().
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [scratch, out_path, working_directory,
         on_done = std::move(on_done)](ProcessUtil::Result result) mutable {
          if (result.has_value()) {
            on_done(BuildOutput{
                .bytes = read_file(out_path),
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
