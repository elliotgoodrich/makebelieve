// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include "iocontext.hpp"
#include "processutilinternal.hpp"
#include "stringutil.hpp"
#include "tracestatus.hpp"
#include "tracingenvironment.hpp"

#include <exec/when_any.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <detours.h>

namespace makebelieve {

namespace {

// Environment variables the injected hook DLL reads: the log to append reads
// to, and the root under which a read counts as a dependency.

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

  // Closes the handle held, if any, and holds @a handle instead.
  void reset(HANDLE handle) {
    if (m_handle != nullptr) {
      CloseHandle(m_handle);
    }
    m_handle = handle;
  }

  ScopedHandle(ScopedHandle&& other) noexcept
      : m_handle(std::exchange(other.m_handle, nullptr)) {}

  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    reset(std::exchange(other.m_handle, nullptr));
    return *this;
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
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

// The run's TraceStatus, in shared memory the command and everything it starts
// open by name.
class SharedStatus {
  ScopedHandle m_mapping;
  TraceStatus* m_status;
  std::wstring m_name;
  ScopedHandle m_failed;

  SharedStatus(ScopedHandle mapping,
               TraceStatus* status,
               std::wstring name,
               ScopedHandle failed)
      : m_mapping(std::move(mapping)),
        m_status(status),
        m_name(std::move(name)),
        m_failed(std::move(failed)) {}

 public:
  // Creates a zeroed status, and its failure event, under a name no other run
  // has, or says why it could not.
  static std::expected<SharedStatus, std::error_code> create() {
    static std::atomic<std::uint64_t> s_next{0};
    std::wstring name =
        std::format(L"Local\\makebelieve-trace-{}-{}", GetCurrentProcessId(),
                    s_next.fetch_add(1, std::memory_order_relaxed));
    ScopedHandle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                            PAGE_READWRITE, 0,
                                            sizeof(TraceStatus), name.c_str()));
    if (mapping.get() == nullptr) {
      return std::unexpected(last_error_code());
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      // Someone else's: its contents cannot be trusted.
      return std::unexpected(std::make_error_code(std::errc::file_exists));
    }
    ScopedHandle failed(
        CreateEventW(nullptr, TRUE, FALSE,
                     (name + TraceStatus::k_failure_event_suffix).c_str()));
    if (failed.get() == nullptr) {
      return std::unexpected(last_error_code());
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      return std::unexpected(std::make_error_code(std::errc::file_exists));
    }
    auto* status = static_cast<TraceStatus*>(
        MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                      sizeof(TraceStatus)));
    if (status == nullptr) {
      return std::unexpected(last_error_code());
    }
    // The command itself, which is launched by us rather than by a hook.
    status->expect_process();
    return SharedStatus(std::move(mapping), status, std::move(name),
                        std::move(failed));
  }

  SharedStatus(SharedStatus&& other) noexcept
      : m_mapping(std::move(other.m_mapping)),
        m_status(std::exchange(other.m_status, nullptr)),
        m_name(std::move(other.m_name)),
        m_failed(std::move(other.m_failed)) {}

  ~SharedStatus() {
    if (m_status != nullptr) {
      UnmapViewOfFile(m_status);
    }
  }

  SharedStatus(const SharedStatus&) = delete;
  SharedStatus& operator=(const SharedStatus&) = delete;
  SharedStatus& operator=(SharedStatus&&) = delete;

  [[nodiscard]] const std::wstring& name() const { return m_name; }

  // Signalled once any hook reports a failure.
  [[nodiscard]] HANDLE failed() const { return m_failed.get(); }

  // Nothing if every process expected installed the hook and none reported
  // a failure, or why what was logged cannot be trusted.
  // @pre Every traced process has exited.
  [[nodiscard]] std::error_code verdict() const {
    if (m_status->any_failed()) {
      // A hook failed to install or to record a read: a lost dependency.
      return std::make_error_code(std::errc::io_error);
    }
    if (m_status->installs() < m_status->expected_processes()) {
      // A process ran untraced, so the log may be missing its reads.
      return std::make_error_code(std::errc::operation_not_supported);
    }
    return {};
  }
};

// Reads the hook's UTF-8, newline-terminated log of read paths, deduplicated
// and sorted. An empty log means the command read nothing; a log that is
// missing, or cannot be read or decoded, is an error rather than an empty list,
// since tracing is required and a lost read is a lost dependency.
std::expected<std::vector<std::filesystem::path>, std::error_code>
read_trace_log(const std::filesystem::path& log) {
  std::ifstream stream(log, std::ios::binary);
  if (!stream) {
    std::error_code ec;
    return std::unexpected(
        std::filesystem::exists(log, ec)
            ? std::make_error_code(std::errc::io_error)
            : std::make_error_code(std::errc::no_such_file_or_directory));
  }
  std::vector<std::filesystem::path> inputs;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    std::expected<std::wstring, std::error_code> wide =
        StringUtil::to_wide(line);
    if (!wide.has_value()) {
      return std::unexpected(wide.error());
    }
    inputs.emplace_back(std::move(*wide));
  }
  if (stream.bad()) {
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }
  std::ranges::sort(inputs);
  inputs.erase(std::ranges::unique(inputs).begin(), inputs.end());
  return inputs;
}

// Creates a job object that terminates every process still in it when its last
// handle closes. Enrolling the command means a stop, or simply returning, tears
// down the whole tree it spawned.
std::expected<ScopedHandle, std::error_code> create_kill_on_close_job() {
  ScopedHandle job(CreateJobObjectW(nullptr, nullptr));
  if (job.get() == nullptr) {
    return std::unexpected(last_error_code());
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    return std::unexpected(last_error_code());
  }
  return job;
}

// The size of the pipe a command's output goes through, and of each read from
// it.
constexpr DWORD k_pipe_bytes = 64 * 1024;

// Creates the pipe a command writes its standard output to: @a read, ours,
// opened for overlapped reads and kept out of the command, and @a write, the
// command's, inheritable. Anonymous pipes cannot be read asynchronously, so it
// is a named pipe with a name no other can take.
std::error_code make_output_pipe(ScopedHandle& read, ScopedHandle& write) {
  static std::atomic<std::uint64_t> next_id{0};
  const std::wstring name = std::format(L"\\\\.\\pipe\\makebelieve-{}-{}",
                                        GetCurrentProcessId(), ++next_id);
  const HANDLE server =
      CreateNamedPipeW(name.c_str(),
                       PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                           FILE_FLAG_FIRST_PIPE_INSTANCE,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                           PIPE_REJECT_REMOTE_CLIENTS,
                       1, 0, k_pipe_bytes, 0, nullptr);
  if (server == INVALID_HANDLE_VALUE) {
    return last_error_code();
  }
  read.reset(server);

  SECURITY_ATTRIBUTES inheritable = {
      .nLength = sizeof(inheritable),
      .bInheritHandle = TRUE,
  };
  const HANDLE client =
      CreateFileW(name.c_str(), GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (client == INVALID_HANDLE_VALUE) {
    return last_error_code();
  }
  write.reset(client);
  return {};
}

// Collects a command's output from our end of its pipe, keeping one
// overlapped read outstanding at a time and signalling event() whenever it
// completes.
class OutputReader {
  HANDLE m_pipe;
  ScopedHandle m_event;
  OVERLAPPED m_overlapped{};
  std::vector<char> m_buffer = std::vector<char>(k_pipe_bytes);
  bool m_pending = false;
  bool m_open = true;
  std::string m_output;

  // Stops the outstanding read, keeping whatever it delivered first.
  void cancel() {
    if (!m_pending) {
      return;
    }
    CancelIoEx(m_pipe, &m_overlapped);
    DWORD bytes = 0;
    if (GetOverlappedResult(m_pipe, &m_overlapped, &bytes, TRUE)) {
      m_output.append(m_buffer.data(), bytes);
    }
    m_pending = false;
  }

 public:
  // Manual reset, so a read that completes before the wait on it starts is
  // not missed.
  explicit OutputReader(HANDLE pipe)
      : m_pipe(pipe), m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

  // The kernel may yet write into m_buffer, so no read may outlive it.
  ~OutputReader() { cancel(); }

  OutputReader(const OutputReader&) = delete;
  OutputReader& operator=(const OutputReader&) = delete;
  OutputReader(OutputReader&&) = delete;
  OutputReader& operator=(OutputReader&&) = delete;

  [[nodiscard]] bool valid() const { return m_event.get() != nullptr; }

  // Signalled whenever the outstanding read completes.
  [[nodiscard]] HANDLE event() const { return m_event.get(); }

  // Whether the command's end of the pipe is still open.
  [[nodiscard]] bool open() const { return m_open; }

  // Keeps what completed reads delivered and issues the next, until one is
  // left outstanding or the pipe has closed.
  void pump() {
    while (m_open) {
      if (m_pending) {
        DWORD bytes = 0;
        if (!GetOverlappedResult(m_pipe, &m_overlapped, &bytes, FALSE)) {
          if (GetLastError() == ERROR_IO_INCOMPLETE) {
            return;  // Still outstanding.
          }
          // ERROR_BROKEN_PIPE is end of file; anything else ends reading too.
          m_pending = false;
          m_open = false;
          return;
        }
        m_pending = false;
        m_output.append(m_buffer.data(), bytes);
      }
      ResetEvent(m_event.get());
      m_overlapped = OVERLAPPED{};
      m_overlapped.hEvent = m_event.get();
      if (!ReadFile(m_pipe, m_buffer.data(),
                    static_cast<DWORD>(m_buffer.size()), nullptr,
                    &m_overlapped) &&
          GetLastError() != ERROR_IO_PENDING) {
        m_open = false;
        return;
      }
      // Finished at once or not, GetOverlappedResult() reports it.
      m_pending = true;
    }
  }

  // Once the command has exited: keeps what the pipe still holds, without
  // waiting on anything it started that still has the pipe open.
  void finish() {
    pump();
    cancel();
  }

  [[nodiscard]] std::string take() && { return std::move(m_output); }
};

// Traces the files one command, and everything it starts, reads under a root:
// the hook DLL injected into them, and the private log the hook appends each
// read to. Prepared before the launch, which it supplies the DLL and the
// environment for, and read back once everything the command started has
// finished.
class TracingSession {
  std::filesystem::path m_log;
  SharedStatus m_status;
  std::string m_hook;
  std::wstring m_environment;

  TracingSession(std::filesystem::path log,
                 SharedStatus status,
                 std::string hook,
                 std::wstring environment)
      : m_log(std::move(log)),
        m_status(std::move(status)),
        m_hook(std::move(hook)),
        m_environment(std::move(environment)) {}

 public:
  // Finds the hook DLL, and creates the log and the status it reports through,
  // for a command run in @a working_directory - or says why it cannot:
  // tracing is required, so any missing fails the run.
  static std::expected<std::unique_ptr<TracingSession>, std::error_code>
  prepare(const std::filesystem::path& working_directory) {
    if (const std::optional<std::error_code> forced =
            detail::take_forced_failure(
                ProcessUtilTestUtil::Failure::tracing_setup)) {
      return std::unexpected(*forced);
    }
    const std::expected<std::filesystem::path, std::error_code> hook =
        find_hook_dll();
    if (!hook.has_value()) {
      return std::unexpected(hook.error());
    }
    std::expected<std::filesystem::path, std::error_code> log =
        make_trace_log();
    if (!log.has_value()) {
      return std::unexpected(log.error());
    }
    std::expected<SharedStatus, std::error_code> status =
        SharedStatus::create();
    if (!status.has_value()) {
      std::error_code ec;
      std::filesystem::remove(*log, ec);
      return std::unexpected(status.error());
    }
    std::wstring_view test_failure;
    if (detail::take_forced_failure(
            ProcessUtilTestUtil::Failure::trace_hook_install)) {
      test_failure = TraceStatus::k_fail_install;
    } else if (detail::take_forced_failure(
                   ProcessUtilTestUtil::Failure::trace_hook_absent)) {
      test_failure = TraceStatus::k_fail_absent;
    } else if (detail::take_forced_failure(ProcessUtilTestUtil::Failure::
                                               trace_hook_absent_in_children)) {
      test_failure = TraceStatus::k_fail_absent_in_children;
    }
    // The hook reports each read in canonical form, so hand it the root in the
    // same form or its under-the-root filter drops everything on a caller
    // whose working directory carries an 8.3 short component.
    const std::wstring root = canonical_directory(working_directory).wstring();
    const std::wstring log_value = log->wstring();
    std::vector<TracingEnvironment::Variable> variables = {
        {TraceStatus::k_log_var, log_value},
        {TraceStatus::k_root_var, root},
        {TraceStatus::k_status_var, status->name()}};
    if (!test_failure.empty()) {
      variables.emplace_back(TraceStatus::k_test_failure_var, test_failure);
    }
    // Any tracing variables this process was itself given, as a traced
    // command would have been, give way to this run's.
    std::wstring environment =
        TracingEnvironment::build(TracingEnvironment::own_entries(), variables);
    return std::unique_ptr<TracingSession>(new TracingSession(
        std::move(*log), std::move(*status),
        StringUtil::to_utf8(hook->wstring()), std::move(environment)));
  }

  ~TracingSession() {
    std::error_code ec;
    std::filesystem::remove(m_log, ec);
  }

  TracingSession(const TracingSession&) = delete;
  TracingSession& operator=(const TracingSession&) = delete;
  TracingSession(TracingSession&&) = delete;
  TracingSession& operator=(TracingSession&&) = delete;

  // The hook DLL to inject, as Detours wants it.
  [[nodiscard]] const std::string& hook() const { return m_hook; }

  // The command's environment block. CreateProcessW may write to it.
  [[nodiscard]] std::wstring& environment() { return m_environment; }

  // Signalled once any hook reports that it failed to record a read.
  [[nodiscard]] HANDLE failed() const { return m_status.failed(); }

  // The files the command and everything it started read - or why they
  // cannot be known: a hook reported a failure, none reported installing, or
  // the log could not be read. @pre Everything traced has exited.
  [[nodiscard]] std::expected<std::vector<std::filesystem::path>,
                              std::error_code>
  take_inputs() const {
    if (const std::error_code error = m_status.verdict()) {
      return std::unexpected(error);
    }
    if (detail::take_forced_failure(ProcessUtilTestUtil::Failure::trace_log)) {
      std::error_code ec;
      std::filesystem::remove(m_log, ec);
    }
    return read_trace_log(m_log);
  }
};

// A running command, in a job of its own along with everything it starts.
// terminate() ends the job and waits, through the IoContext, until every
// process in it has exited. Closing the job - when this is destroyed - asks
// the system to kill whatever is left, as a last resort, but does not wait
// for it.
//
// How long terminate() can take is bounded by the processes themselves: it
// waits on each one it can open, which the system kills promptly, and polls
// for any it cannot open only until a short deadline, after which it reports
// that their exit could not be confirmed.
class ChildProcess {
  ScopedHandle m_job;
  ScopedHandle m_process;
  ScopedHandle m_thread;

  ChildProcess(ScopedHandle job, ScopedHandle process, ScopedHandle thread)
      : m_job(std::move(job)),
        m_process(std::move(process)),
        m_thread(std::move(thread)) {}

  // The processes still in the job, opened to be waited on - or why the job
  // could not be asked. Those that have exited already, or cannot be opened,
  // are left out.
  [[nodiscard]] std::expected<std::vector<ScopedHandle>, std::error_code>
  open_processes_left() const {
    constexpr DWORD k_max_ids = 256;
    std::vector<std::byte> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
                                  k_max_ids * sizeof(ULONG_PTR));
    auto* list =
        reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    // More than fit are waited on a batch at a time, so a partial list will
    // do.
    if (!QueryInformationJobObject(m_job.get(), JobObjectBasicProcessIdList,
                                   list, static_cast<DWORD>(buffer.size()),
                                   nullptr) &&
        GetLastError() != ERROR_MORE_DATA) {
      return std::unexpected(last_error_code());
    }
    std::vector<ScopedHandle> handles;
    for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
      if (const HANDLE handle = OpenProcess(
              SYNCHRONIZE, FALSE, static_cast<DWORD>(list->ProcessIdList[i]));
          handle != nullptr) {
        handles.emplace_back(handle);
      }
    }
    return handles;
  }

  // How many processes are still in the job, or why the job could not say.
  [[nodiscard]] std::expected<DWORD, std::error_code> active_processes() const {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(m_job.get(),
                                   JobObjectBasicAccountingInformation,
                                   &accounting, sizeof(accounting), nullptr)) {
      return std::unexpected(last_error_code());
    }
    return accounting.ActiveProcesses;
  }

 public:
  // How to start a command.
  struct Launch {
    std::wstring& command_line;  // CreateProcessW may write to it
    std::wstring& environment;   // as may it to this
    const std::string& hook;     // the DLL to inject, as Detours wants it
    const std::filesystem::path& working_directory;
    HANDLE standard_output;
  };

  // Starts the command @a spec describes, with the hook it names injected,
  // or says why it could not. A command that cannot be enrolled in a job never
  // runs: without one, what it started could be neither ended nor waited for.
  static std::expected<std::unique_ptr<ChildProcess>, std::error_code> launch(
      const Launch& spec) {
    // Created before the suspended launch so the command is enrolled before it
    // can start anything.
    std::expected<ScopedHandle, std::error_code> job =
        create_kill_on_close_job();
    if (!job.has_value()) {
      return std::unexpected(job.error());
    }

    STARTUPINFOW startup = {
        .cb = sizeof(startup),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
        .hStdOutput = spec.standard_output,
        .hStdError = GetStdHandle(STD_ERROR_HANDLE),
    };

    // Suspended, so the command is enrolled in the job before its code runs.
    // Detours injects into the suspended process and, given CREATE_SUSPENDED,
    // leaves the resume to us. The hook is in place before the command's code
    // runs, and re-injects into anything it starts.
    const DWORD creation_flags =
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    PROCESS_INFORMATION info{};
    if (!DetourCreateProcessWithDllExW(nullptr, spec.command_line.data(),
                                       nullptr, nullptr, TRUE, creation_flags,
                                       spec.environment.data(),
                                       spec.working_directory.c_str(), &startup,
                                       &info, spec.hook.c_str(), nullptr)) {
      return std::unexpected(last_error_code());
    }
    ScopedHandle process(info.hProcess);
    ScopedHandle thread(info.hThread);

    // Either failure leaves the command suspended, so it is ended - promptly -
    // before any of its code has run.
    const auto abandon = [&process]() {
      const std::error_code error = last_error_code();
      TerminateProcess(process.get(), 1);
      WaitForSingleObject(process.get(), INFINITE);
      return std::unexpected(error);
    };
    if (!AssignProcessToJobObject(job->get(), process.get())) {
      return abandon();
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
      return abandon();
    }
    return std::unique_ptr<ChildProcess>(new ChildProcess(
        std::move(*job), std::move(process), std::move(thread)));
  }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&&) = delete;
  ChildProcess& operator=(ChildProcess&&) = delete;
  ~ChildProcess() = default;

  // Signalled once the command itself - cmd.exe - has exited.
  [[nodiscard]] HANDLE exited() const { return m_process.get(); }

  // The status the command exited with, or why it could not be read.
  // @pre It has exited.
  [[nodiscard]] std::expected<int, std::error_code> exit_code() const {
    DWORD code = 0;
    if (!GetExitCodeProcess(m_process.get(), &code)) {
      return std::unexpected(last_error_code());
    }
    return static_cast<int>(code);
  }

  // Asks for the command, and everything it started, to be killed.
  void kill() const noexcept { TerminateJobObject(m_job.get(), 1); }

  // Kills the command and everything it started, then waits through @a io
  // until every one of them has exited. Completes with nothing once that is
  // confirmed, or with why it could not be: `std::errc::timed_out` if a
  // process that cannot be opened outlived the deadline, or the error that
  // kept the job from being ended, asked what is left in it, or waited on.
  exec::task<std::error_code> terminate(IoContext& io) {
    const auto wait = [&io](HANDLE handle) {
      return stdexec::write_env(io.async_wait(handle),
                                stdexec::prop{stdexec::get_stop_token,
                                              stdexec::never_stop_token{}}) |
             stdexec::then([]() noexcept { return std::error_code{}; }) |
             stdexec::upon_error(
                 [](std::error_code error) noexcept { return error; });
    };
    if (!TerminateJobObject(m_job.get(), 1)) {
      // Nothing in the tree will exit for us to wait on: end cmd.exe at least,
      // wait for that alone, and report that the rest was not ended.
      const std::error_code error = last_error_code();
      TerminateProcess(m_process.get(), 1);
      co_await wait(m_process.get());
      co_return error;
    }
    // Termination takes effect asynchronously, so wait on whatever is still
    // in the job until nothing is. A process there that cannot be opened -
    // its access denies it - cannot be waited on, so it is polled for, but
    // only until the deadline, so that one that also never exits cannot hold
    // the run forever.
    constexpr auto k_unopenable_deadline = std::chrono::seconds(2);
    const auto deadline =
        std::chrono::steady_clock::now() + k_unopenable_deadline;
    while (true) {
      const std::expected<DWORD, std::error_code> active = active_processes();
      if (!active.has_value()) {
        co_return active.error();
      }
      if (*active == 0) {
        co_return std::error_code{};
      }
      const std::expected<std::vector<ScopedHandle>, std::error_code> left =
          open_processes_left();
      if (!left.has_value()) {
        co_return left.error();
      }
      if (left->empty()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          co_return std::make_error_code(std::errc::timed_out);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      for (const ScopedHandle& handle : *left) {
        if (const std::error_code error = co_await wait(handle.get())) {
          co_return error;
        }
      }
    }
  }
};

// Kills @a child on stop.
struct Kill {
  const ChildProcess* child;

  void operator()() const noexcept { child->kill(); }
};

}  // namespace

exec::task<ProcessUtil::Result> ProcessUtil::run_task(
    IoContext& io,
    std::filesystem::path working_directory,
    std::string command,
    stdexec::inplace_stop_token stop) {
  if (stop.stop_requested()) {
    co_return std::unexpected(
        std::make_error_code(std::errc::operation_canceled));
  }

  std::expected<std::unique_ptr<TracingSession>, std::error_code> tracing =
      TracingSession::prepare(working_directory);
  if (!tracing.has_value()) {
    co_return std::unexpected(tracing.error());
  }

  ScopedHandle read_end(nullptr);
  ScopedHandle write_end(nullptr);
  if (const std::error_code error = make_output_pipe(read_end, write_end)) {
    co_return std::unexpected(error);
  }
  OutputReader reader(read_end.get());
  if (!reader.valid()) {
    co_return std::unexpected(last_error_code());
  }

  const std::expected<std::wstring, std::error_code> wcommand =
      StringUtil::to_wide(command);
  if (!wcommand.has_value()) {
    co_return std::unexpected(wcommand.error());
  }
  // CreateProcessW takes a mutable command line.
  std::wstring command_line = L"cmd.exe /c " + *wcommand;

  std::expected<std::unique_ptr<ChildProcess>, std::error_code> child =
      ChildProcess::launch({.command_line = command_line,
                            .environment = (*tracing)->environment(),
                            .hook = (*tracing)->hook(),
                            .working_directory = working_directory,
                            .standard_output = write_end.get()});
  // Close our copy of the command's end, so only it can write to the pipe and
  // the pipe closes once it has gone.
  write_end.reset(nullptr);
  if (!child.has_value()) {
    co_return std::unexpected(child.error());
  }

  // Collect output until the command exits, or tracing fails - after which
  // it has lost a dependency, so ending it at once keeps it from holding its
  // build slot for however long it would otherwise run. Waiting on the
  // process rather than on the pipe closing is what keeps anything it started
  // that inherited the pipe from wedging us, and reading as it arrives keeps a
  // full pipe from blocking the command.
  std::error_code wait_error;
  {
    const stdexec::inplace_stop_callback<Kill> on_stop(stop,
                                                       Kill{child->get()});
    const auto failed = [&wait_error](std::error_code error) noexcept {
      wait_error = error;
      return -1;
    };
    constexpr int k_output = 0;
    constexpr int k_exited = 1;
    constexpr int k_tracing_failed = 2;
    reader.pump();
    while (true) {
      auto exited = io.async_wait((*child)->exited()) |
                    stdexec::then([]() noexcept { return k_exited; }) |
                    stdexec::upon_error(failed);
      auto tracing_failed =
          io.async_wait((*tracing)->failed()) |
          stdexec::then([]() noexcept { return k_tracing_failed; }) |
          stdexec::upon_error(failed);
      const int which =
          reader.open()
              ? co_await exec::when_any(
                    io.async_wait(reader.event()) |
                        stdexec::then([]() noexcept { return k_output; }) |
                        stdexec::upon_error(failed),
                    std::move(exited), std::move(tracing_failed))
              : co_await exec::when_any(std::move(exited),
                                        std::move(tracing_failed));
      if (which != k_output) {
        // Ended, with everything it started, by terminate() below; the
        // verdict then reports why.
        break;
      }
      reader.pump();
    }
  }
  // Whatever the command wrote just before it exited.
  reader.finish();

  // Everything the command started has gone too, and so has finished writing
  // to the trace log, once this confirms it.
  const std::error_code terminate_error = co_await (*child)->terminate(io);

  if (wait_error) {
    co_return std::unexpected(wait_error);
  }
  if (stop.stop_requested()) {
    co_return std::unexpected(
        std::make_error_code(std::errc::operation_canceled));
  }
  const std::expected<int, std::error_code> exit_code = (*child)->exit_code();
  if (!exit_code.has_value()) {
    co_return std::unexpected(exit_code.error());
  }
  // Something the command started may still be writing to the trace log.
  if (terminate_error) {
    co_return std::unexpected(terminate_error);
  }
  std::expected<std::vector<std::filesystem::path>, std::error_code> inputs =
      (*tracing)->take_inputs();
  if (!inputs.has_value()) {
    co_return std::unexpected(inputs.error());
  }
  co_return Output{.standard_output = std::move(reader).take(),
                   .inputs = std::move(*inputs),
                   .exit_status = *exit_code};
}

}  // namespace makebelieve
