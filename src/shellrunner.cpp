// SPDX-License-Identifier: MIT
#include "shellrunner.hpp"

#include "directorytreeutil.hpp"
#include "filesystemutil.hpp"
#include "iocontext.hpp"
#include "manifest.hpp"
#include "nativefile.hpp"
#include "processutil.hpp"
#include "scopeguard.hpp"
#include "tracer.hpp"

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <random>
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

class BuildErrorCategory : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "build"; }

  [[nodiscard]] std::string message(int error) const override {
    switch (static_cast<BuildError>(error)) {
      case BuildError::out_not_written:
        return "command did not write %out";
      case BuildError::copy_source_unreadable:
        return "could not read the file to copy";
    }
    return "unknown build error";
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

// Creates a fresh directory in @a base, or says why it could not. Only a name
// that is taken already is retried.
std::expected<std::filesystem::path, std::error_code> make_temp_directory(
    const std::filesystem::path& base) {
  std::error_code ec;
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

// @a error, met reading the file @a path, put as the same error on every
// platform for the two a missing file gives: that it does not exist, or is a
// directory.
std::error_code explain(std::error_code error,
                        const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::file_status status = std::filesystem::status(path, ec);
  if (ec) {
    return ec;
  }
  if (!std::filesystem::exists(status)) {
    return std::make_error_code(std::errc::no_such_file_or_directory);
  }
  if (std::filesystem::is_directory(status)) {
    return std::make_error_code(std::errc::is_a_directory);
  }
  return error;
}

// Why a file of type @a type cannot be taken for a build's output.
std::error_code not_a_file(std::filesystem::file_type type) {
  switch (type) {
    case std::filesystem::file_type::not_found:
      return std::make_error_code(std::errc::no_such_file_or_directory);
    case std::filesystem::file_type::directory:
      return std::make_error_code(std::errc::is_a_directory);
    default:
      return std::make_error_code(std::errc::invalid_argument);
  }
}

// Creates @a to holding the bytes of the file @a from, or says why it could
// not. Neither is inherited by a command launched meanwhile.
std::error_code copy_into(const std::filesystem::path& from,
                          const std::filesystem::path& to) {
  const std::expected<std::shared_ptr<const NativeFile>, std::error_code>
      source = NativeFile::open(from);
  return source.has_value() ? DirectoryTreeUtil::write_file(to, **source)
                            : source.error();
}

// Moves the file @a written, which a `run` command left at %out, to @a kept,
// or says why it could not - that there is no file there, say. A symlink
// left there is taken for the file it names, whose bytes are copied, as a
// reader would take it: what it names may go with the build's directory.
std::error_code take(const std::filesystem::path& written,
                     const std::filesystem::path& kept) {
  std::error_code ec;
  const std::filesystem::file_type type =
      std::filesystem::symlink_status(written, ec).type();
  if (type == std::filesystem::file_type::regular) {
    std::filesystem::rename(written, kept, ec);
    return ec;
  }
  if (type != std::filesystem::file_type::symlink) {
    return not_a_file(type);
  }
  const std::filesystem::file_type target =
      std::filesystem::status(written, ec).type();
  return target == std::filesystem::file_type::regular
             ? copy_into(written, kept)
             : not_a_file(target);
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

// @a diagnostics - what a command wrote, if anything - followed by @a reason on
// a line of its own: the standard error of a build that failed for a reason
// the command itself did not give, so whoever reads the failure finds out.
std::string with_reason(std::string diagnostics, std::string_view reason) {
  if (!diagnostics.empty() && !diagnostics.ends_with('\n')) {
    diagnostics += '\n';
  }
  diagnostics += reason;
  diagnostics += '\n';
  return diagnostics;
}

}  // namespace

const std::error_category& exit_status_category() {
  static const ExitStatusCategory category;
  return category;
}

const std::error_category& build_error_category() {
  static const BuildErrorCategory category;
  return category;
}

exec::task<BuildResult> detail::run_shell_command(
    IoContext& io,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named by the header
    std::filesystem::path working_directory,
    std::filesystem::path mountpoint,
    std::filesystem::path scratch_directory,
    Command command,
    BuildProcessTracker& tracker,
    stdexec::inplace_stop_token stop) {
  MB_TRACE_THREAD_NAME("build worker");

  // Every build gets a private directory in the scratch directory, which goes
  // with whatever the build leaves in it, and what it builds ends up in a
  // file beside that directory - so with a name that cannot collide - for the
  // tree to take over. A `run` command builds into a file inside the
  // directory with the output's own file name, since plenty of tools (pandoc
  // and friends) pick their format from the extension, which is then moved
  // out; the other actions write that file directly.
  std::expected<std::filesystem::path, std::error_code> directory =
      make_temp_directory(scratch_directory);
  if (!directory.has_value()) {
    co_return std::unexpected(directory.error());
  }
  const TempDirectory scratch(std::move(*directory));
  std::filesystem::path kept = scratch.path();
  kept += ".out";
  // Unless it is handed over, as the build fails.
  ScopeGuard discard_kept([&kept] {
    std::error_code ignored;
    std::filesystem::remove(kept, ignored);
  });

  // A `copy` rule names a file rather than a command: nothing is run, the
  // output is that file's bytes, and the file itself is the build's one input -
  // so a copy tracks its source even on a platform where command tracing is
  // unavailable. The parser has already held the path to one inside the
  // working directory.
  if (command.action == Manifest::Action::Copy) {
    const std::filesystem::path source = command.text;
    // The reader sees the output, so the source's own error - that it does
    // not exist, say - would read as being about the output. The source is
    // still its input, so creating or fixing it retries the copy: a tracer
    // records only the files that opened, so nothing else would.
    if (const std::error_code error =
            copy_into(working_directory / source, kept)) {
      co_return std::unexpected(BuildFailure(
          make_error_code(BuildError::copy_source_unreadable), {source},
          with_reason(
              {}, "cannot copy [" + source.generic_string() + "]: " +
                      explain(error, working_directory / source).message())));
    }
    discard_kept.dismiss();
    co_return BuildOutput{.file = std::move(kept), .inputs = {source}};
  }

  // A `run` command has its file's path substituted in for %out, and its
  // build output is whatever it left there - not its standard output, which
  // a `> %out` rule leaves empty. A `capture` command's standard output is
  // its output, so goes straight to the file handed over.
  const bool capture = command.action == Manifest::Action::Capture;
  const std::filesystem::path out_path =
      scratch.path() / command.output.filename();
  std::string text = expand_generated(command.text, mountpoint);
  if (!capture) {
    text = substitute_out(text, out_path);
  }
  ProcessUtil::Result result = co_await ProcessUtil::run(
      io, working_directory, text, tracker, command.output, stop,
      CaptureOptions{
          .standard_output = k_kept_diagnostics,
          .standard_error = k_kept_diagnostics,
          .standard_output_file = capture ? kept : std::filesystem::path()});
  if (!result.has_value()) {
    co_return std::unexpected(result.error());
  }
  // A `run` command's standard output is not its output, but may say why it
  // failed - MSVC's tools give their errors there - so it is kept with its
  // standard error, ahead of it, as how the two interleaved is lost. No more
  // than k_kept_diagnostics of the two is, with a line saying how much more
  // there was.
  std::string diagnostics;
  std::size_t omitted = result->standard_error_omitted;
  if (!capture) {
    diagnostics = std::move(result->standard_output);
    omitted += result->standard_output_omitted;
  }
  diagnostics += result->standard_error;
  if (diagnostics.size() > k_kept_diagnostics) {
    omitted += diagnostics.size() - k_kept_diagnostics;
    diagnostics.resize(k_kept_diagnostics);
  }
  if (omitted != 0) {
    diagnostics =
        with_reason(std::move(diagnostics),
                    "... and " + std::to_string(omitted) + " more bytes");
  }
  // What it read, whether or not it built anything: a failure is retried when
  // one of these changes, as a success is rebuilt.
  std::vector<std::filesystem::path> inputs =
      relativize(result->inputs, working_directory);
  const auto fail = [&inputs](std::error_code error,
                              std::string standard_error) {
    return std::unexpected(
        BuildFailure(error, std::move(inputs), std::move(standard_error)));
  };
  // A command that reports failure has not built its output.
  if (result->exit_status != 0) {
    co_return fail(exit_status_error(result->exit_status),
                   std::move(diagnostics));
  }
  // A command that left nothing at %out has not built its output. Reading
  // the scratch file's own error back - that it does not exist, say - would
  // read as being about the output.
  if (const std::error_code error =
          capture ? std::error_code() : take(out_path, kept)) {
    co_return fail(
        make_error_code(BuildError::out_not_written),
        with_reason(
            std::move(diagnostics),
            error == std::errc::no_such_file_or_directory
                ? std::string(
                      "command exited successfully but wrote nothing to %out")
                : "command exited successfully but its %out could not be "
                  "read: " +
                      error.message()));
  }
  discard_kept.dismiss();
  // What it printed is dropped: it built, so there is nothing to explain.
  co_return BuildOutput{.file = std::move(kept), .inputs = std::move(inputs)};
}

}  // namespace makebelieve
