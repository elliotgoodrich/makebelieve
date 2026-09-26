// SPDX-License-Identifier: MIT
#include "shellrunner.hpp"

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

using Command = BuildDirectoryTree::Command;
using BuildOutput = BuildDirectoryTree::BuildOutput;
using BuildResult = BuildDirectoryTree::BuildResult;

// The placeholder in a command that is replaced with the output's path.
constexpr std::string_view k_out_placeholder = "%out";

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

// Carries out @a command as `ShellRunner` describes,
// waiting on a command it runs through @a io. @a stop cancels a command still
// running.
exec::task<BuildResult> run_shell_command(
    IoContext& io,
    std::filesystem::path working_directory,
    Command command,
    stdexec::inplace_stop_token stop) {
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
    scratch.emplace(make_temp_directory());
    out_path = scratch->path() / command.output.filename();
  }

  ProcessUtil::Result result = co_await ProcessUtil::run(
      io, working_directory,
      capture ? command.text : substitute_out(command.text, out_path), stop);
  if (!result.has_value()) {
    co_return std::unexpected(result.error());
  }
  co_return BuildOutput{
      .bytes = capture ? std::move(result->standard_output)
                       : read_file(out_path).value_or(std::string{}),
      .inputs = relativize(result->inputs, working_directory)};
}

}  // namespace

ShellRunner::ShellRunner(std::filesystem::path working_directory,
                         IoContext& io,
                         exec::static_thread_pool::scheduler workers)
    : m_working_directory(std::move(working_directory)),
      m_io(&io),
      m_workers(workers) {}

BuildDirectoryTree::BuildSender ShellRunner::operator()(Command command) const {
  // Started on the pool, where the task then resumes after each wait, so its
  // synchronous steps never run on the IoContext's thread. Spelled as
  // schedule-then-let_value because stdexec cannot type-erase a starts_on onto
  // a static_thread_pool.
  return stdexec::schedule(m_workers) |
         stdexec::let_value([io = m_io, working_directory = m_working_directory,
                             command = std::move(command)]() mutable {
           return stdexec::read_env(stdexec::get_stop_token) |
                  stdexec::let_value([&](stdexec::inplace_stop_token stop) {
                    MB_TRACE_THREAD_NAME("build worker");
                    return run_shell_command(*io, working_directory,
                                             std::move(command), stop);
                  });
         });
}

}  // namespace makebelieve
