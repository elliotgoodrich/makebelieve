// SPDX-License-Identifier: MIT
#include "shellrunner.hpp"

#include "iocontext.hpp"
#include "manifest.hpp"
#include "processutil.hpp"
#include "tracer.hpp"

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace makebelieve {

namespace {

class ExitStatusCategory : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override {
    return "exit status";
  }

  [[nodiscard]] std::string message(int status) const override {
    return "command exited with status " + std::to_string(status);
  }
};

// The placeholder in a command that is replaced with the output's path.
constexpr std::string_view k_out_placeholder = "%out";

// The prefix in a command naming another output, replaced with its path
// through the mount.
constexpr std::string_view k_generated_prefix = "@/";

// Besides whitespace, what ends an unquoted `@/` path.
constexpr std::string_view k_word_enders = "\"'<>|;&()";

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

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

// Creates a fresh directory under the system's temporary one, or says why it
// could not. Only a name that is taken already is retried.
std::expected<std::filesystem::path, std::error_code> make_temp_directory() {
  std::error_code ec;
  const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return std::unexpected(ec);
  }
  std::random_device device;
  std::uniform_int_distribution<unsigned> digit(0, 0xffffffU);
  constexpr int k_attempts = 100;
  for (int attempt = 0; attempt < k_attempts; ++attempt) {
    std::filesystem::path candidate =
        base / ("makebelieve-build-" + std::to_string(digit(device)));
    if (std::filesystem::create_directory(candidate, ec)) {
      return candidate;
    }
    // A directory already there is reported as nothing created; a file
    // there, as file_exists. Anything else will not go away by retrying.
    if (ec && ec != std::errc::file_exists) {
      return std::unexpected(ec);
    }
  }
  return std::unexpected(std::make_error_code(std::errc::file_exists));
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

// Replaces every `@/<path>` in @a command with that output's path under
// @a mountpoint, using native separators (cmd's built-ins read `/` as a
// switch). An unquoted path is quoted, as %out is; one already inside quotes
// runs to the closing quote.
std::string expand_generated(std::string_view command,
                             const std::filesystem::path& mountpoint) {
  // Without a trailing separator.
  const std::string root =
      (mountpoint.has_filename() ? mountpoint : mountpoint.parent_path())
          .string();
  std::string result;
  char quote = 0;
  std::size_t pos = 0;
  while (pos < command.size()) {
    const char c = command[pos];
    if (!command.substr(pos).starts_with(k_generated_prefix)) {
      if (c == quote) {
        quote = 0;
      } else if (quote == 0 && (c == '"' || c == '\'')) {
        quote = c;
      }
      result += c;
      ++pos;
      continue;
    }

    const std::size_t start = pos + k_generated_prefix.size();
    std::size_t end = start;
    while (end < command.size() &&
           (quote != 0 ? command[end] != quote
                       : !is_space(command[end]) &&
                             !k_word_enders.contains(command[end]))) {
      ++end;
    }
    std::string path = root;
    if (end != start) {
      path += std::filesystem::path::preferred_separator;
      for (const char part : command.substr(start, end - start)) {
        path +=
            part == '/'
                ? static_cast<char>(std::filesystem::path::preferred_separator)
                : part;
      }
    }
    result += quote != 0 ? path : "\"" + path + "\"";
    pos = end;
  }
  return result;
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

}  // namespace

const std::error_category& exit_status_category() {
  static const ExitStatusCategory category;
  return category;
}

exec::task<BuildResult> detail::run_shell_command(
    IoContext& io,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named by the header
    std::filesystem::path working_directory,
    std::filesystem::path mountpoint,
    Command command,
    LaunchRegistrar registrar,
    stdexec::inplace_stop_token stop) {
  MB_TRACE_THREAD_NAME("build worker");

  // A `copy` rule names a file rather than a command: nothing is run, the
  // output is that file's bytes, and the file itself is the build's one input -
  // so a copy tracks its source even on a platform where command tracing is
  // unavailable. The parser has already held the path to one inside the
  // working directory.
  if (command.action == Manifest::Action::Copy) {
    const std::filesystem::path source = command.text;
    std::expected<std::string, std::error_code> bytes =
        read_file(working_directory / source);
    if (!bytes.has_value()) {
      co_return std::unexpected(bytes.error());
    }
    co_return BuildOutput{.bytes = std::move(*bytes), .inputs = {source}};
  }

  const bool capture = command.action == Manifest::Action::Capture;

  // A `run` command gets a private file to write its output to - with the
  // output's own file name, since plenty of tools (pandoc and friends) pick
  // their format from the extension - with that path substituted in for %out.
  // The directory is fresh per build, so the name cannot collide. Its build
  // output is whatever it left in that file - not ProcessUtil's captured
  // stdout, which a `> %out` rule leaves empty. A `capture` command has no
  // output file: its stdout is the output.
  std::optional<TempDirectory> scratch;
  std::filesystem::path out_path;
  if (!capture) {
    std::expected<std::filesystem::path, std::error_code> directory =
        make_temp_directory();
    if (!directory.has_value()) {
      co_return std::unexpected(directory.error());
    }
    scratch.emplace(std::move(*directory));
    out_path = scratch->path() / command.output.filename();
  }

  std::string text = expand_generated(command.text, mountpoint);
  if (!capture) {
    text = substitute_out(text, out_path);
  }
  ProcessUtil::Result result =
      co_await ProcessUtil::run(io, working_directory, text, registrar, stop);
  if (!result.has_value()) {
    co_return std::unexpected(result.error());
  }
  // A command that reports failure has not built its output.
  if (result->exit_status != 0) {
    co_return std::unexpected(exit_status_error(result->exit_status));
  }
  std::string bytes;
  if (capture) {
    bytes = std::move(result->standard_output);
  } else {
    // A command that left nothing at %out has not built its output.
    std::expected<std::string, std::error_code> written = read_file(out_path);
    if (!written.has_value()) {
      co_return std::unexpected(written.error());
    }
    bytes = std::move(*written);
  }
  co_return BuildOutput{
      .bytes = std::move(bytes),
      .inputs = relativize(result->inputs, working_directory)};
}

}  // namespace makebelieve
