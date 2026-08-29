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
  // The directory structure and file contents we present. Each declared
  // output starts life as a placeholder (see k_placeholder_content); its real
  // content replaces the placeholder once its command reports success.
  InMemoryDirectoryTree m_structure;

  // Outputs that still need building, mapped to the command that builds them.
  // An entry is erased only once its output has been built successfully, so a
  // command that fails is retried on the next read.
  std::map<std::filesystem::path, std::string> m_pending;

  // Outputs whose build has been started and is awaiting its completion; keeps
  // a second read from launching the same command while the first is in flight.
  std::set<std::filesystem::path> m_in_flight;

  CommandRunner m_runner;

  // Requested on destruction to tell in-flight runners their results are no
  // longer wanted.
  std::stop_source m_stop;

 public:
  Impl(const DirectoryTree& source, CommandRunner runner)
      : m_runner(std::move(runner)) {
    const std::optional<std::string> manifest =
        read_all(source, k_manifest_name);
    if (!manifest.has_value()) {
      return;
    }

    const Manifest parsed = Manifest::parse(*manifest);
    for (const Manifest::Rule& rule : parsed.rules()) {
      ensure_parent_directories(m_structure, rule.output);
      m_structure.write_file(rule.output, k_placeholder_content);
      m_pending.insert_or_assign(rule.output, rule.command);
    }
  }

  ~Impl() { m_stop.request_stop(); }

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

  // The first read of a declared output hands its command to the runner; the
  // completion swaps the placeholder for the real result. Reads taken before
  // that completes (or while another is in flight) see the placeholder.
  [[nodiscard]] std::expected<std::string, std::error_code>
  read(const std::filesystem::path& path, Offset offset, std::size_t size) {
    // A zero-size read never opens the file (so never triggers a build), and a
    // negative offset is a caller error the structure will reject; in both
    // cases building first would be pointless work.
    if (size != 0 && offset >= 0) {
      start_build(path.lexically_normal());
    }
    return m_structure.read(path, offset, size);
  }

  [[nodiscard]] Subscription subscribe_to_changes(
      const std::function<void(const DirectoryTreeDiff&)>& callback) const {
    return m_structure.subscribe_to_changes(callback);
  }

 private:
  void start_build(const std::filesystem::path& output) {
    const auto pending = m_pending.find(output);
    if (pending == m_pending.end() || m_in_flight.contains(output)) {
      return;
    }
    m_in_flight.insert(output);

    // A synchronous runner reports back inside this call, so the read that
    // triggered the build already sees the finished content; an asynchronous
    // one reports later and the placeholder stands until it does.
    m_runner(pending->second, m_stop.get_token(),
             // A completion legitimately allocates (it writes the built bytes
             // into the structure); a synchronous runner lets that propagate
             // out of read().
             // NOLINTNEXTLINE(bugprone-exception-escape)
             [this, output](BuildResult result) {
               finish_build(output, std::move(result));
             });
  }

  void finish_build(const std::filesystem::path& output, BuildResult result) {
    m_in_flight.erase(output);
    if (result.has_value()) {
      m_structure.write_file(output, std::move(*result));
      m_pending.erase(output);
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

    ProcessUtil::run(working_directory, substitute_out(command, out_path), stop,
                     // Reading the output allocates; a synchronous ProcessUtil
                     // lets that propagate back out to the triggering read().
                     // NOLINTNEXTLINE(bugprone-exception-escape)
                     [scratch, out_path, on_done = std::move(on_done)](
                         ProcessUtil::Result result) mutable {
                       if (result.has_value()) {
                         on_done(read_file(out_path));
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
