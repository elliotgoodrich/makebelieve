// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include "stringutil.hpp"

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

  // Closes the handle held, if any, and holds @a handle instead.
  void reset(HANDLE handle) {
    if (m_handle != nullptr) {
      CloseHandle(m_handle);
    }
    m_handle = handle;
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&&) = delete;
  ScopedHandle& operator=(ScopedHandle&&) = delete;
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
  std::string m_hook;
  std::wstring m_environment;

  TracingSession(std::filesystem::path log,
                 std::string hook,
                 std::wstring environment)
      : m_log(std::move(log)),
        m_hook(std::move(hook)),
        m_environment(std::move(environment)) {}

 public:
  // Finds the hook DLL and creates the log for a command run in
  // @a working_directory, or says why it cannot: tracing is required, so
  // either missing fails the run.
  static std::expected<std::unique_ptr<TracingSession>, std::error_code>
  prepare(const std::filesystem::path& working_directory) {
    if (const std::optional<std::error_code> forced = ProcessUtilTestUtil::take(
            static_cast<int>(ProcessUtilTestUtil::Failure::tracing_setup))) {
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
    // The hook reports each read in canonical form, so hand it the root in the
    // same form or its under-the-root filter drops everything on a caller
    // whose working directory carries an 8.3 short component.
    std::wstring environment = child_environment_with_trace(
        log->wstring(), canonical_directory(working_directory).wstring());
    return std::unique_ptr<TracingSession>(new TracingSession(
        std::move(*log), StringUtil::to_utf8(hook->wstring()),
        std::move(environment)));
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

  // The files the command and everything it started read, or why the log
  // could not be read.
  [[nodiscard]] std::expected<std::vector<std::filesystem::path>,
                              std::error_code>
  take_inputs() const {
    if (ProcessUtilTestUtil::take(
            static_cast<int>(ProcessUtilTestUtil::Failure::trace_log))) {
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
// for any it cannot open only until a short deadline, after which their exit
// is requested but not confirmed.
class ChildProcess {
  ScopedHandle m_job;
  ScopedHandle m_process;
  ScopedHandle m_thread;
  bool m_in_job;

  ChildProcess(HANDLE job, HANDLE process, HANDLE thread, bool in_job)
      : m_job(job), m_process(process), m_thread(thread), m_in_job(in_job) {}

  // The processes still in the job, opened to be waited on. Those that have
  // exited already, or cannot be opened, are left out.
  [[nodiscard]] std::vector<HANDLE> open_processes_left() const {
    constexpr DWORD k_max_ids = 256;
    std::vector<std::byte> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
                                  k_max_ids * sizeof(ULONG_PTR));
    auto* list =
        reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    std::vector<HANDLE> handles;
    if (!QueryInformationJobObject(m_job.get(), JobObjectBasicProcessIdList,
                                   list, static_cast<DWORD>(buffer.size()),
                                   nullptr) &&
        GetLastError() != ERROR_MORE_DATA) {
      return handles;
    }
    for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
      if (const HANDLE handle = OpenProcess(
              SYNCHRONIZE, FALSE, static_cast<DWORD>(list->ProcessIdList[i]));
          handle != nullptr) {
        handles.push_back(handle);
      }
    }
    return handles;
  }

  // How many processes are still in the job.
  [[nodiscard]] DWORD active_processes() const {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(m_job.get(),
                                   JobObjectBasicAccountingInformation,
                                   &accounting, sizeof(accounting), nullptr)) {
      return 0;
    }
    return accounting.ActiveProcesses;
  }

 public:
  // Starts @a command_line, traced by @a tracing, in @a working_directory with
  // its standard output on @a standard_output, or says why it could not.
  static std::expected<std::unique_ptr<ChildProcess>, std::error_code> launch(
      std::wstring& command_line,
      TracingSession& tracing,
      const std::filesystem::path& working_directory,
      HANDLE standard_output) {
    // Created before the suspended launch so the command is enrolled before it
    // can start anything. nullptr falls back to ending cmd.exe alone.
    const HANDLE job = create_kill_on_close_job();

    STARTUPINFOW startup = {
        .cb = sizeof(startup),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
        .hStdOutput = standard_output,
        .hStdError = GetStdHandle(STD_ERROR_HANDLE),
    };

    // Suspended, so the command is enrolled in the job before its code runs.
    // Detours injects into the suspended process and, given CREATE_SUSPENDED,
    // leaves the resume to us. The hook is in place before the command's code
    // runs, and re-injects into anything it starts.
    const DWORD creation_flags =
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    PROCESS_INFORMATION process{};
    if (!DetourCreateProcessWithDllExW(
            nullptr, command_line.data(), nullptr, nullptr, TRUE,
            creation_flags, tracing.environment().data(),
            working_directory.c_str(), &startup, &process,
            tracing.hook().c_str(), nullptr)) {
      const std::error_code error = last_error_code();
      if (job != nullptr) {
        CloseHandle(job);
      }
      return std::unexpected(error);
    }

    // If enrolment fails the command still runs, and ending it falls back to
    // terminating cmd.exe alone.
    const bool in_job =
        job != nullptr && AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    return std::unique_ptr<ChildProcess>(
        new ChildProcess(job, process.hProcess, process.hThread, in_job));
  }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&&) = delete;
  ChildProcess& operator=(ChildProcess&&) = delete;
  ~ChildProcess() = default;

  // Signalled once the command itself - cmd.exe - has exited.
  [[nodiscard]] HANDLE exited() const { return m_process.get(); }

  // Asks for the command, and everything it started, to be killed.
  void kill() const noexcept {
    if (m_in_job) {
      TerminateJobObject(m_job.get(), 1);
    } else {
      TerminateProcess(m_process.get(), 1);
    }
  }

  // Kills the command and everything it started, then waits through @a io
  // until every one of them has exited - or, for any it cannot open, until a
  // short deadline passes.
  exec::task<void> terminate(IoContext& io) {
    const auto wait = [&io](HANDLE handle) {
      return stdexec::write_env(io.async_wait(handle),
                                stdexec::prop{stdexec::get_stop_token,
                                              stdexec::never_stop_token{}}) |
             stdexec::upon_error([](std::error_code) noexcept {});
    };
    if (!m_in_job || !TerminateJobObject(m_job.get(), 1)) {
      // The tree's termination cannot be requested, so nothing in it will
      // exit for us to wait on: end cmd.exe, and wait for that alone.
      TerminateProcess(m_process.get(), 1);
      co_await wait(m_process.get());
      co_return;
    }
    // Termination takes effect asynchronously, so wait on whatever is still
    // in the job until nothing is. A process there that cannot be opened -
    // its access denies it - cannot be waited on, so it is polled for, but
    // only until the deadline, so that one that also never exits cannot hold
    // the run forever.
    constexpr auto k_unopenable_deadline = std::chrono::seconds(2);
    const auto deadline =
        std::chrono::steady_clock::now() + k_unopenable_deadline;
    while (active_processes() != 0) {
      const std::vector<HANDLE> left = open_processes_left();
      if (left.empty()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          co_return;  // Requested, not confirmed.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      for (const HANDLE handle : left) {
        co_await wait(handle);
      }
      for (const HANDLE handle : left) {
        CloseHandle(handle);
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
      ChildProcess::launch(command_line, **tracing, working_directory,
                           write_end.get());
  // Close our copy of the command's end, so only it can write to the pipe and
  // the pipe closes once it has gone.
  write_end.reset(nullptr);
  if (!child.has_value()) {
    co_return std::unexpected(child.error());
  }

  // Collect output until the command exits. Waiting on the process rather
  // than on the pipe closing is what keeps anything it started that inherited
  // the pipe from wedging us, and reading as it arrives keeps a full pipe from
  // blocking the command.
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
    reader.pump();
    while (true) {
      auto exited = io.async_wait((*child)->exited()) |
                    stdexec::then([]() noexcept { return k_exited; }) |
                    stdexec::upon_error(failed);
      const int which =
          reader.open()
              ? co_await exec::when_any(
                    io.async_wait(reader.event()) |
                        stdexec::then([]() noexcept { return k_output; }) |
                        stdexec::upon_error(failed),
                    exited)
              : co_await exited;
      if (which != k_output) {
        break;
      }
      reader.pump();
    }
  }
  // Whatever the command wrote just before it exited.
  reader.finish();

  // Everything the command started has gone too, and so has finished writing
  // to the trace log, once this returns.
  co_await (*child)->terminate(io);

  if (wait_error) {
    co_return std::unexpected(wait_error);
  }
  if (stop.stop_requested()) {
    co_return std::unexpected(
        std::make_error_code(std::errc::operation_canceled));
  }
  std::expected<std::vector<std::filesystem::path>, std::error_code> inputs =
      (*tracing)->take_inputs();
  if (!inputs.has_value()) {
    co_return std::unexpected(inputs.error());
  }
  co_return Output{.standard_output = std::move(reader).take(),
                   .inputs = std::move(*inputs)};
}

namespace {

// The file name of the executable the process @a id is running, or nothing
// when it has exited or may not be queried.
std::optional<std::string> executable_name(std::uint32_t id) {
  const ScopedHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id));
  if (process.get() == nullptr) {
    return std::nullopt;
  }
  std::array<wchar_t, MAX_PATH> buffer{};
  auto size = static_cast<DWORD>(buffer.size());
  if (QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size) == 0) {
    return std::nullopt;
  }
  return StringUtil::to_utf8(
      std::filesystem::path(std::wstring_view(buffer.data(), size))
          .filename()
          .wstring());
}

}  // namespace

std::uint32_t ProcessUtil::self() {
  return GetCurrentProcessId();
}

std::string ProcessUtil::name_of(std::uint32_t pid) {
  return executable_name(pid).value_or("unknown");
}

}  // namespace makebelieve
