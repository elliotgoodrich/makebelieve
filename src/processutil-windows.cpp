// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include "stringutil.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <detours.h>

namespace makebelieve {

namespace {

// Environment variables the injected hook DLL reads: the log to append reads
// to, and the root under which a read counts as a dependency.
constexpr wchar_t k_trace_log_var[] = L"MAKEBELIEVE_TRACE_LOG";
constexpr wchar_t k_trace_root_var[] = L"MAKEBELIEVE_TRACE_ROOT";

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

// Closes a HANDLE on scope exit, tolerating a null handle.
class ScopedHandle {
  HANDLE m_handle;

 public:
  explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}

  ~ScopedHandle() {
    if (m_handle != nullptr) {
      CloseHandle(m_handle);
    }
  }

  [[nodiscard]] HANDLE get() const { return m_handle; }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&&) = delete;
  ScopedHandle& operator=(ScopedHandle&&) = delete;
};

// Removes a file on scope exit.
class ScopedFile {
  std::filesystem::path m_path;

 public:
  explicit ScopedFile(const std::filesystem::path& path) : m_path(path) {}

  ~ScopedFile() {
    std::error_code ec;
    std::filesystem::remove(m_path, ec);
  }

  ScopedFile(const ScopedFile&) = delete;
  ScopedFile& operator=(const ScopedFile&) = delete;
  ScopedFile(ScopedFile&&) = delete;
  ScopedFile& operator=(ScopedFile&&) = delete;
};

// Locates makebelievehook.dll next to the running executable. Fails if absent,
// since tracing is mandatory.
std::expected<std::filesystem::path, std::error_code> find_hook_dll() {
  std::array<wchar_t, 32768> self{};
  const DWORD len =
      GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
  if (len == 0 || len >= self.size()) {
    return std::unexpected(last_error_code());
  }
  std::filesystem::path path(std::wstring_view(self.data(), len));
  path.replace_filename(L"makebelievehook.dll");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return std::unexpected(ec);
  }
  return path;
}

// Creates a uniquely-named empty temp file for the hook's log. GetTempFileNameW
// names it atomically, so concurrent runs cannot collide.
std::expected<std::filesystem::path, std::error_code> make_trace_log() {
  std::array<wchar_t, MAX_PATH + 1> dir{};
  const DWORD n = GetTempPathW(static_cast<DWORD>(dir.size()), dir.data());
  if (n == 0 || n > dir.size()) {
    return std::unexpected(last_error_code());
  }
  std::array<wchar_t, MAX_PATH + 1> file{};
  if (GetTempFileNameW(dir.data(), L"mbt", 0, file.data()) == 0) {
    return std::unexpected(last_error_code());
  }
  return std::filesystem::path(file.data());
}

// Resolves @a directory to the canonical form the hook derives for the files it
// opens (via GetFinalPathNameByHandleW), so the two line up when it filters
// reads against the traced root - in particular expanding any 8.3 short-name
// components, which a caller's temp path can carry. Returns @a directory
// unchanged if it cannot be opened or resolved.
std::filesystem::path canonical_directory(
    const std::filesystem::path& directory) {
  const HANDLE handle =
      CreateFileW(directory.c_str(), 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return directory;
  }
  const ScopedHandle guard(handle);
  std::array<wchar_t, 32768> buffer{};
  const DWORD len = GetFinalPathNameByHandleW(
      handle, buffer.data(), static_cast<DWORD>(buffer.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (len == 0 || len >= buffer.size()) {
    return directory;
  }
  std::wstring_view resolved(buffer.data(), len);
  constexpr std::wstring_view k_extended_prefix = L"\\\\?\\";
  if (resolved.starts_with(k_extended_prefix)) {
    resolved.remove_prefix(k_extended_prefix.size());
  }
  return std::filesystem::path(resolved);
}

// Builds a child environment block: the parent's plus the two trace variables.
// A per-child block keeps the trace paths off the shared process environment,
// so concurrent runs cannot clobber each other's values.
std::wstring child_environment_with_trace(const std::wstring& log_value,
                                          const std::wstring& root_value) {
  std::wstring block;
  if (const LPWCH env = GetEnvironmentStringsW(); env != nullptr) {
    for (const wchar_t* entry = env; *entry != L'\0';) {
      const std::wstring_view view(entry);
      entry += view.size() + 1;
      // Drop inherited copies; ours are appended below.
      if (view.starts_with(L"MAKEBELIEVE_TRACE_LOG=") ||
          view.starts_with(L"MAKEBELIEVE_TRACE_ROOT=")) {
        continue;
      }
      block.append(view);
      block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
  }
  block.append(k_trace_log_var).append(L"=").append(log_value).push_back(L'\0');
  block.append(k_trace_root_var)
      .append(L"=")
      .append(root_value)
      .push_back(L'\0');
  block.push_back(L'\0');  // the block itself is double-null terminated
  return block;
}

// Reads the hook's UTF-8, newline-terminated log of read paths, deduplicated
// and sorted. A missing or empty log yields an empty list.
std::vector<std::filesystem::path> read_trace_log(
    const std::filesystem::path& log) {
  std::vector<std::filesystem::path> inputs;
  std::ifstream stream(log, std::ios::binary);
  if (!stream) {
    return inputs;
  }
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (std::expected<std::wstring, std::error_code> wide =
            StringUtil::to_wide(line);
        wide.has_value()) {
      inputs.emplace_back(std::move(*wide));
    }
  }
  std::ranges::sort(inputs);
  inputs.erase(std::ranges::unique(inputs).begin(), inputs.end());
  return inputs;
}

// Creates a job object that terminates every process still in it when its last
// handle closes. Enrolling the command means a stop, or simply returning, tears
// down the whole tree it spawned. Returns nullptr on failure.
HANDLE create_kill_on_close_job() {
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    return nullptr;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) {
    CloseHandle(job);
    return nullptr;
  }
  return job;
}

}  // namespace

void ProcessUtil::run(const std::filesystem::path& working_directory,
                      const std::string& command,
                      const std::stop_token& stop,
                      Complete on_done) {
  // A pipe carries the child's standard output back to us; the write end is
  // inheritable so the child receives it, the read end is not.
  SECURITY_ATTRIBUTES attributes = {
      .nLength = sizeof(attributes),
      .bInheritHandle = TRUE,
  };

  HANDLE read_raw = nullptr;
  HANDLE write_raw = nullptr;
  if (!CreatePipe(&read_raw, &write_raw, &attributes, 0)) {
    on_done(std::unexpected(last_error_code()));
    return;
  }
  const ScopedHandle read_handle(read_raw);
  SetHandleInformation(read_raw, HANDLE_FLAG_INHERIT, 0);

  // CreateProcessW takes a mutable command-line buffer, so this must be a
  // writable std::wstring rather than a literal.
  std::expected<std::wstring, std::error_code> wcommand =
      StringUtil::to_wide(command);
  if (!wcommand) {
    on_done(std::unexpected(wcommand.error()));
    return;
  }

  // Launch with the hook DLL injected to trace the command's reads. Both the
  // DLL and a private log file are required; failing to find either fails the
  // run.
  const std::expected<std::filesystem::path, std::error_code> hook =
      find_hook_dll();
  if (!hook.has_value()) {
    on_done(std::unexpected(hook.error()));
    return;
  }
  std::expected<std::filesystem::path, std::error_code> trace_log =
      make_trace_log();
  if (!trace_log.has_value()) {
    on_done(std::unexpected(trace_log.error()));
    return;
  }
  const ScopedFile log_cleanup(*trace_log);

  // The hook reports each read in canonical form, so hand it the root in the
  // same form or its under-the-root filter drops everything on a caller whose
  // working directory carries an 8.3 short component.
  // The hook reports each read in canonical form, so hand it the root in the
  // same form or its under-the-root filter drops everything on a caller whose
  // working directory carries an 8.3 short component.
  std::wstring environment = child_environment_with_trace(
      trace_log->wstring(), canonical_directory(working_directory).wstring());

  // Create the job before the suspended launch so the child is enrolled before
  // it can spawn anything. Destroyed last, so closing it kills any process
  // still running; nullptr falls back to ending cmd.exe alone.
  const ScopedHandle job(create_kill_on_close_job());

  std::wstring command_line = L"cmd.exe /c " + wcommand.value();
  STARTUPINFOW startup = {
      .cb = sizeof(startup),
      .dwFlags = STARTF_USESTDHANDLES,
      .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
      .hStdOutput = write_raw,
      .hStdError = GetStdHandle(STD_ERROR_HANDLE),
  };

  // Suspended, so the child is enrolled in the job before its code runs.
  // Detours injects into the suspended process and, given CREATE_SUSPENDED,
  // leaves the resume to us.
  DWORD creation_flags =
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;

  PROCESS_INFORMATION process{};
  // The hook is in place before the command's code runs, and re-injects into
  // any child the command spawns.
  const std::string hook_utf8 = StringUtil::to_utf8(hook->wstring());
  const BOOL created = DetourCreateProcessWithDllExW(
      nullptr, command_line.data(), nullptr, nullptr, TRUE, creation_flags,
      environment.data(), working_directory.c_str(), &startup, &process,
      hook_utf8.c_str(), nullptr);
  const std::error_code create_error =
      created ? std::error_code{} : last_error_code();
  // Close our copy of the write end so the pipe reports end of file once the
  // child exits, and so only the child can write to it.
  CloseHandle(write_raw);
  if (!created) {
    on_done(std::unexpected(create_error));
    return;
  }

  const ScopedHandle thread(process.hThread);
  const ScopedHandle handle(process.hProcess);

  // Enrol in the job, then run. If enrolment fails the command still runs, and
  // a stop falls back to terminating cmd.exe alone.
  const bool in_job = job.get() != nullptr &&
                      AssignProcessToJobObject(job.get(), process.hProcess);
  ResumeThread(process.hThread);

  // Terminate on stop: the job tears down the whole tree, else just cmd.exe.
  // The callback is declared after the handles so it can no longer fire once
  // they close, avoiding a reused-id race. Runs synchronously if stop is
  // already set.
  const std::stop_callback on_stop(stop, [process, job = job.get(), in_job] {
    if (in_job) {
      TerminateJobObject(job, 1);
    } else {
      TerminateProcess(process.hProcess, 1);
    }
  });

  std::string output;
  std::array<char, 4096> buffer{};
  const auto drain = [&] {
    while (true) {
      DWORD available = 0;
      if (!PeekNamedPipe(read_raw, nullptr, 0, nullptr, &available, nullptr) ||
          available == 0) {
        return;
      }
      const DWORD want = std::min(available, static_cast<DWORD>(buffer.size()));
      DWORD read_bytes = 0;
      if (!ReadFile(read_raw, buffer.data(), want, &read_bytes, nullptr) ||
          read_bytes == 0) {
        return;
      }
      output.append(buffer.data(), read_bytes);
    }
  };

  // Drain the pipe until the direct child exits. Waiting on the process rather
  // than on pipe end-of-file is what keeps an inherited-pipe grandchild from
  // wedging us, and draining as we go keeps a full pipe from blocking writes.
  std::error_code wait_error;
  while (true) {
    drain();
    const DWORD waited = WaitForSingleObject(process.hProcess, 50);
    if (waited == WAIT_OBJECT_0) {
      drain();  // Whatever the child buffered just before it exited.
      break;
    }
    if (waited == WAIT_FAILED) {
      wait_error = last_error_code();
      break;
    }
  }

  if (wait_error) {
    on_done(std::unexpected(wait_error));
  } else if (stop.stop_requested()) {
    on_done(
        std::unexpected(std::make_error_code(std::errc::operation_canceled)));
  } else {
    // cmd.exe has waited for the command, so every traced process' synchronous
    // appends are already on disk.
    on_done(Output{.standard_output = std::move(output),
                   .inputs = read_trace_log(*trace_log)});
  }
}

}  // namespace makebelieve
