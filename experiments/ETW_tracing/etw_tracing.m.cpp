/**
 * @file
 * Runs a command under an NT Kernel Logger ETW trace and prints every file
 * under the current directory that the command, or a descendant process it
 * spawns, opened for reading.
 *
 * Unlike FUSE_tracing, ETW has no mount boundary: it's a system-wide event
 * stream, not a virtual filesystem, so nothing about the mechanism itself
 * confines it to the traced process or to paths under the current
 * directory. Both boundaries are reconstructed here in user mode instead:
 *
 *   - Process boundary: seed a traced-PID set with the launched process's
 *     PID, and grow it from Process-start events whose parent PID is
 *     already in the set (the "process attribution" approach), so
 *     descendant processes are covered - something FUSE_tracing gets for
 *     free from being mount-scoped rather than process-scoped, and this
 *     tracer has to do by hand.
 *   - Path boundary: keep only events whose resolved path falls under the
 *     current directory, the same as FUSE_tracing's own report - but
 *     computed by string comparison here, not by construction.
 *
 * ETW is also asynchronous and its buffers are lossy under load - a burst
 * of accesses can overflow before this process's consumer thread drains
 * them (see the file-I/O event loop's own MSDN remarks on buffer loss).
 * This tracer can therefore under-report compared to FUSE_tracing; it
 * never over-reports. See the project's own design discussion for why
 * that asymmetry is acceptable for a "trust, then verify by stat"
 * dependency tracer but not for one that needs to synchronously gate
 * opens.
 *
 * Requires running elevated (Administrator or a member of "Performance Log
 * Users"): consuming NT Kernel Logger events is privileged, regardless of
 * how unprivileged the traced command itself is.
 *
 * Usage:
 * @code
 *   etw_tracing_experiment <command> [args...]
 * @endcode
 */

// windows.h must precede evntrace.h/tdh.h - they rely on basic types and
// calling-convention macros (NTAPI, ...) windows.h defines, and don't
// include it themselves. INITGUID must precede evntrace.h too, to turn its
// DEFINE_GUID entries (SystemTraceControlGuid among them) into real
// constants instead of just declarations - see kProcessGuid/kFileIoGuid
// below for the two kernel GUIDs this header no longer defines at all.
#include <windows.h>

#define INITGUID
#include <evntrace.h>
#undef INITGUID
#include <tdh.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Current Windows SDKs no longer ship these two as ready-made constants
// the way SystemTraceControlGuid still is (verified against a real
// evntrace.h - INITGUID above only materializes what the header actually
// still DEFINE_GUIDs; ProcessGuid/FileIoGuid aren't among them there, even
// though Microsoft's own docs describe them as living in this header), so
// they're defined by hand here, from the byte values published at
// https://learn.microsoft.com/en-us/windows/win32/etw/nt-kernel-logger-constants.
constexpr GUID kProcessGuid = {
    0x3d6fa8d0,
    0xfe05,
    0x11d0,
    {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kFileIoGuid = {0x90cbdc39,
                              0x4a3e,
                              0x11d1,
                              {0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3}};

/// Maps an NT-native volume device path (e.g. `\Device\HarddiskVolume3`, as
/// returned by QueryDosDeviceW) to its drive letter - FileIo_Create's
/// OpenPath property is reported in NT-native form, not the familiar
/// `C:\...` form, so this is needed to turn a reported path back into
/// something comparable against std::filesystem::current_path(). Sorted
/// longest-prefix-first so a volume path that happens to prefix another
/// one is never matched short.
std::vector<std::pair<std::wstring, wchar_t>> build_drive_prefix_map() {
  std::vector<std::pair<std::wstring, wchar_t>> result;
  const DWORD drive_bits = ::GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    if ((drive_bits & (1u << (letter - L'A'))) == 0) {
      continue;
    }
    const wchar_t drive[3] = {letter, L':', L'\0'};
    wchar_t target[MAX_PATH];
    if (::QueryDosDeviceW(drive, target, MAX_PATH) != 0) {
      result.emplace_back(target, letter);
    }
  }
  std::ranges::sort(result, [](const auto& a, const auto& b) {
    return a.first.size() > b.first.size();
  });
  return result;
}

std::wstring to_lower(std::wstring_view s) {
  std::wstring result(s);
  std::ranges::transform(result, result.begin(), ::towlower);
  return result;
}

/// Looks up a single top-level property by name via TDH, working for the
/// classic MOF-based kernel events (FileIo_Create, Process_TypeGroup1, ...)
/// the same way it would for a manifest-based provider - TDH's whole point
/// is to unify decoding across both. Returns std::nullopt if the event
/// doesn't carry a property of that name (e.g. asking a FileIo_ReadWrite
/// event for "OpenPath", which only FileIo_Create events have).
std::optional<std::vector<BYTE>> get_property_raw(EVENT_RECORD* event,
                                                  PCWSTR name) {
  PROPERTY_DATA_DESCRIPTOR pdd{};
  pdd.PropertyName = reinterpret_cast<ULONGLONG>(name);
  ULONG size = 0;
  if (::TdhGetPropertySize(event, 0, nullptr, 1, &pdd, &size) !=
          ERROR_SUCCESS ||
      size == 0) {
    return std::nullopt;
  }
  std::vector<BYTE> buffer(size);
  if (::TdhGetProperty(event, 0, nullptr, 1, &pdd, size, buffer.data()) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  return buffer;
}

/// The FileIo/Process MOF classes declare their string properties as
/// StringTermination("NullTerminated"), Format("w") - i.e. an
/// already-null-terminated wide string - so the raw TdhGetProperty buffer
/// can be used directly with no separate TdhFormatProperty pass.
std::optional<std::wstring> get_string_property(EVENT_RECORD* event,
                                                PCWSTR name) {
  const std::optional<std::vector<BYTE>> raw = get_property_raw(event, name);
  if (!raw.has_value() || raw->empty()) {
    return std::nullopt;
  }
  return std::wstring(reinterpret_cast<const wchar_t*>(raw->data()));
}

std::optional<DWORD> get_uint32_property(EVENT_RECORD* event, PCWSTR name) {
  const std::optional<std::vector<BYTE>> raw = get_property_raw(event, name);
  if (!raw.has_value() || raw->size() < sizeof(DWORD)) {
    return std::nullopt;
  }
  DWORD value = 0;
  std::memcpy(&value, raw->data(), sizeof(value));
  return value;
}

/// Consumes the NT Kernel Logger's real-time event stream, reconstructing
/// the process and path boundaries described in the file comment.
class Tracer {
  std::filesystem::path m_root;
  std::wstring m_root_prefix_lower;  // m_root's string form, lowercased, with a
                                     // trailing separator
  std::vector<std::pair<std::wstring, wchar_t>> m_drive_prefixes;

  std::mutex m_mutex;
  std::unordered_set<DWORD> m_traced_pids;
  std::vector<std::filesystem::path> m_candidates;

 public:
  explicit Tracer(std::filesystem::path root)
      : m_root(std::move(root)), m_drive_prefixes(build_drive_prefix_map()) {
    std::wstring prefix = m_root.wstring();
    if (!prefix.empty() && prefix.back() != L'\\') {
      prefix.push_back(L'\\');
    }
    m_root_prefix_lower = to_lower(prefix);
  }

  Tracer(const Tracer&) = delete;
  Tracer& operator=(const Tracer&) = delete;

  static void WINAPI on_event(EVENT_RECORD* event) {
    static_cast<Tracer*>(event->UserContext)->handle_event(event);
  }

  /// Adds a PID to the traced set. Called once, from main(), with the
  /// traced command's own PID - while it's still suspended, so no file
  /// event of its own can arrive before this runs - after which the set
  /// only ever grows itself, from handle_process_event() below.
  void add_traced_pid(DWORD pid) {
    const std::scoped_lock lock(m_mutex);
    m_traced_pids.insert(pid);
  }

  std::vector<std::filesystem::path> take_candidates() && {
    return std::move(m_candidates);
  }

 private:
  void handle_event(EVENT_RECORD* event) {
    if (event->EventHeader.ProviderId == kProcessGuid) {
      handle_process_event(event);
    } else if (event->EventHeader.ProviderId == kFileIoGuid) {
      handle_fileio_event(event);
    }
  }

  /// Grows m_traced_pids from Process_TypeGroup1 events (Start/DCStart and
  /// also End/Stop, harmlessly - a PID that's already exited by the time
  /// it's added just never matches a later FileIo event's ProcessId)
  /// whose ParentId is already a traced PID. This is what lets a build
  /// action's own child processes be traced, unlike a raw single-PID
  /// filter.
  void handle_process_event(EVENT_RECORD* event) {
    const std::optional<DWORD> pid = get_uint32_property(event, L"ProcessId");
    const std::optional<DWORD> parent_pid =
        get_uint32_property(event, L"ParentId");
    if (!pid.has_value() || !parent_pid.has_value()) {
      return;
    }
    const std::scoped_lock lock(m_mutex);
    if (m_traced_pids.contains(*parent_pid)) {
      m_traced_pids.insert(*pid);
    }
  }

  /// Records a path from a FileIo_Create event (EventType 64, the one that
  /// carries OpenPath - see the FileIo class remarks) once it passes the
  /// process and path boundary checks. Every other FileIo event type
  /// (reads, writes, directory enumeration, the type-32/0 FileCreate/Name
  /// events, ...) is deliberately ignored: FileIo_Create is logged at
  /// open time for every opened file - directory or not - which is
  /// verified separately at report time in main() via GetFileAttributesW,
  /// mirroring FUSE_tracing's own directories-never-reported behavior
  /// without needing to duplicate NtCreateFile's CreateOptions decoding
  /// here.
  void handle_fileio_event(EVENT_RECORD* event) {
    {
      const std::scoped_lock lock(m_mutex);
      if (!m_traced_pids.contains(event->EventHeader.ProcessId)) {
        return;
      }
    }

    const std::optional<std::wstring> open_path =
        get_string_property(event, L"OpenPath");
    if (!open_path.has_value() || open_path->empty()) {
      return;
    }
    const std::optional<std::filesystem::path> resolved =
        resolve_native_path(*open_path);
    if (!resolved.has_value()) {
      return;
    }

    const std::wstring lower = to_lower(resolved->wstring());
    if (lower.size() <= m_root_prefix_lower.size() ||
        lower.compare(0, m_root_prefix_lower.size(), m_root_prefix_lower) !=
            0) {
      return;  // outside the current directory - not tracked, same as ".."
    }

    const std::scoped_lock lock(m_mutex);
    m_candidates.push_back(*resolved);
  }

  /// Rewrites an NT-native path (`\Device\HarddiskVolume3\...`) to its
  /// drive-letter form, or passes through a path that's already in
  /// drive-letter form. Returns std::nullopt for anything else (a UNC
  /// path, an unrecognized device, ...) rather than guessing - an omitted
  /// dependency is a silent under-report, matching this tracer's general
  /// lossiness, but a mis-rewritten path would be actively misleading.
  [[nodiscard]] std::optional<std::filesystem::path> resolve_native_path(
      std::wstring_view raw) const {
    if (raw.size() >= 2 && raw[1] == L':') {
      return std::filesystem::path(raw);
    }
    for (const auto& [prefix, letter] : m_drive_prefixes) {
      if (raw.size() > prefix.size() &&
          raw.compare(0, prefix.size(), prefix) == 0 &&
          raw[prefix.size()] == L'\\') {
        std::wstring rewritten;
        rewritten.push_back(letter);
        rewritten.push_back(L':');
        rewritten += raw.substr(prefix.size());
        return std::filesystem::path(rewritten);
      }
    }
    return std::nullopt;
  }
};

/// Bundles the two handles a running trace needs: the session itself
/// (created by StartTraceW, which is what's actually collecting events) and
/// the real-time consumer handle (opened by OpenTraceW, which is what
/// ProcessTrace reads from). Both are required to stop things cleanly.
/// CONTROLTRACE_ID/PROCESSTRACE_HANDLE, not the classic TRACEHANDLE these
/// APIs used to share - current evntrace.h has demoted TRACEHANDLE to
/// "Obsolete - prefer PROCESSTRACE_HANDLE or CONTROLTRACE_ID", each now
/// with its own more specific type, even though both are still plain
/// ULONG64 underneath.
struct TraceSession {
  CONTROLTRACE_ID session = 0;
  PROCESSTRACE_HANDLE consumer = INVALID_PROCESSTRACE_HANDLE;
};

/// Starts the NT Kernel Logger session with the file-I/O and process flags
/// this tracer needs, and opens a real-time consumer handle bound to
/// `tracer`. On any failure, prints a diagnostic and tears down whatever
/// was already started before returning std::nullopt - in particular,
/// ERROR_ALREADY_EXISTS means another NT Kernel Logger session (WPR,
/// xperf, another instance of this tool, ...) is already running, since
/// only one may exist system-wide; this fails outright rather than trying
/// to share or steal it.
std::optional<TraceSession> start_trace_session(Tracer& tracer) {
  const ULONG buffer_size =
      sizeof(EVENT_TRACE_PROPERTIES) + sizeof(KERNEL_LOGGER_NAME);
  std::vector<BYTE> storage(buffer_size, 0);
  auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
  props->Wnode.BufferSize = buffer_size;
  props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  props->Wnode.ClientContext = 1;  // QPC clock resolution
  props->Wnode.Guid = SystemTraceControlGuid;
  props->EnableFlags = EVENT_TRACE_FLAG_DISK_FILE_IO |
                       EVENT_TRACE_FLAG_FILE_IO |
                       EVENT_TRACE_FLAG_FILE_IO_INIT | EVENT_TRACE_FLAG_PROCESS;
  props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
  props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  props->LogFileNameOffset = 0;  // no on-disk log file - real-time only

  TraceSession result;
  ULONG status = ::StartTraceW(&result.session, KERNEL_LOGGER_NAME, props);
  if (status != ERROR_SUCCESS) {
    if (status == ERROR_ALREADY_EXISTS) {
      std::fwprintf(stderr,
                    L"etw_tracing_experiment: an NT Kernel Logger session "
                    L"is already running (WPR/xperf/another instance of "
                    L"this tool?); only one can exist at a time\n");
    } else if (status == ERROR_ACCESS_DENIED) {
      std::fwprintf(stderr,
                    L"etw_tracing_experiment: access denied starting the "
                    L"trace session - this needs to run elevated "
                    L"(Administrator or \"Performance Log Users\")\n");
    } else {
      std::fwprintf(
          stderr, L"etw_tracing_experiment: StartTraceW failed: %lu\n", status);
    }
    return std::nullopt;
  }

  EVENT_TRACE_LOGFILEW logfile{};
  logfile.LoggerName = const_cast<LPWSTR>(KERNEL_LOGGER_NAME);
  logfile.ProcessTraceMode =
      PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
  logfile.EventRecordCallback = &Tracer::on_event;
  logfile.Context = &tracer;

  result.consumer = ::OpenTraceW(&logfile);
  if (result.consumer == INVALID_PROCESSTRACE_HANDLE) {
    std::fwprintf(stderr, L"etw_tracing_experiment: OpenTraceW failed: %lu\n",
                  ::GetLastError());
    ::ControlTraceW(result.session, KERNEL_LOGGER_NAME, props,
                    EVENT_TRACE_CONTROL_STOP);
    return std::nullopt;
  }
  return result;
}

/// Stops a session started by start_trace_session(): ControlTrace(STOP)
/// asks the session to stop, which drains and then unblocks the real-time
/// ProcessTrace() call servicing `session.consumer` on its own thread;
/// CloseTrace() afterward is a defensive second unblock (and always
/// required to release the consumer handle itself) in case ProcessTrace
/// hasn't already returned on its own by the time this runs. Best-effort,
/// matching this repo's other tracers' teardown helpers - a failure here
/// doesn't change whether tracing itself succeeded.
void stop_trace_session(const TraceSession& session) {
  const ULONG buffer_size =
      sizeof(EVENT_TRACE_PROPERTIES) + sizeof(KERNEL_LOGGER_NAME);
  std::vector<BYTE> storage(buffer_size, 0);
  auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
  props->Wnode.BufferSize = buffer_size;
  props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  props->Wnode.Guid = SystemTraceControlGuid;
  props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  ::ControlTraceW(session.session, KERNEL_LOGGER_NAME, props,
                  EVENT_TRACE_CONTROL_STOP);
  ::CloseTrace(session.consumer);
}

/// Quotes a single argument for CreateProcessW's lpCommandLine. Identical
/// to ../detours_tracing/detours_tracing.m.cpp's own copy - each
/// experiment under experiments/ is self-contained (see the top-level
/// CMakeLists.txt comment), so this small helper is duplicated rather
/// than shared.
std::wstring quote_argument(std::wstring_view arg) {
  if (!arg.empty() &&
      arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(arg);
  }
  std::wstring result = L"\"";
  for (auto it = arg.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      result.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(*it);
    } else {
      result.append(backslashes, L'\\');
      result.push_back(*it);
    }
  }
  result.push_back(L'"');
  return result;
}

std::wstring build_command_line(const std::vector<wchar_t*>& argv) {
  std::wstring cmd;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      cmd.push_back(L' ');
    }
    cmd += quote_argument(argv[i]);
  }
  return cmd;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc < 2) {
    std::fwprintf(stderr, L"usage: %s <command> [args...]\n", argv[0]);
    return 2;
  }

  const std::filesystem::path cwd = std::filesystem::current_path();

  Tracer tracer(cwd);

  // Not const: ProcessTrace takes a non-const PROCESSTRACE_HANDLE*, even
  // though the servicing thread below only ever reads session->consumer.
  std::optional<TraceSession> session = start_trace_session(tracer);
  if (!session.has_value()) {
    return 1;
  }

  std::thread servicing(
      [&] { ::ProcessTrace(&session->consumer, 1, nullptr, nullptr); });

  const std::vector<wchar_t*> command(argv + 1, argv + argc);
  std::wstring command_line = build_command_line(command);

  // CREATE_SUSPENDED so the traced command can't do any file I/O of its
  // own before its PID is registered below - otherwise an access in the
  // narrow window between CreateProcessW returning and add_traced_pid()
  // running would arrive at handle_fileio_event() with a PID the tracer
  // doesn't recognize yet, and be silently dropped.
  //
  // bInheritHandles=TRUE so the traced command's own stdout/stderr reach
  // wherever this process's own do - console attachment alone would cover
  // a real console, but not a caller that's redirected our own stdout to
  // a pipe (e.g. a test harness capturing output), which only handle
  // inheritance propagates.
  STARTUPINFOW si{.cb = sizeof(si)};
  PROCESS_INFORMATION pi{};
  const BOOL created =
      ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                       CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi);

  int exit_code = 1;
  if (!created) {
    std::fwprintf(stderr,
                  L"etw_tracing_experiment: failed to start command: error "
                  L"%lu\n",
                  ::GetLastError());
  } else {
    tracer.add_traced_pid(pi.dwProcessId);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    exit_code = static_cast<int>(code);
  }

  stop_trace_session(*session);
  servicing.join();

  std::vector<std::filesystem::path> read_files =
      std::move(tracer).take_candidates();

  // Final existence/regular-file check: FileIo_Create fires for
  // directories too (see handle_fileio_event's comment), and a resolved
  // path can be stale by the time tracing stops (deleted mid-run, or a
  // drive-letter rewrite that was subtly wrong). Silently dropping either
  // case here is the right default for a best-effort tracer - showing a
  // wrong path would be worse than omitting it.
  std::erase_if(read_files, [](const std::filesystem::path& p) {
    const DWORD attrs = ::GetFileAttributesW(p.c_str());
    return attrs == INVALID_FILE_ATTRIBUTES ||
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  });

  std::ranges::sort(read_files);
  read_files.erase(std::ranges::unique(read_files).begin(), read_files.end());

  std::wprintf(L"Files read from %s:\n", cwd.c_str());
  for (const std::filesystem::path& p : read_files) {
    std::wprintf(L"  %s\n", p.c_str());
  }
  std::fflush(stdout);

  return exit_code;
}
