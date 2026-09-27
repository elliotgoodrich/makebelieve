// SPDX-License-Identifier: MIT
/**
 * @file
 * The payload DLL makebelieve injects into a build command, and transitively
 * every child process it spawns, to discover which files the command read. It
 * detours ntdll's NtCreateFile/NtOpenFile and CreateProcessW/CreateProcessA:
 *
 *   - NtCreateFile/NtOpenFile: records the resolved path of every successful,
 *     read-access, non-directory open under the traced root (the log path and
 *     root come from environment variables), appending it to a shared log file.
 *     These are the two ntdll stubs every user-mode file open funnels through,
 *     so hooking here sees them all.
 *   - CreateProcessW/A: re-injects this DLL into every child process, so a
 * build action that shells out to other tools is traced too.
 *
 * Findings go to a log file, appended synchronously inside the hooked call, so
 * by the time the launcher's wait returns every write this process and any
 * exited descendant made is durably on disk. Multiple processes append safely:
 * a handle opened FILE_APPEND_DATA-only gets atomic append from the filesystem.
 *
 * Each process reports on itself through the run's TraceStatus: that it
 * installed the hooks, or that it could not append a read to the log - which
 * loses a dependency, so the run fails rather than trusting the log. A process
 * whose hooks cannot be installed never runs its own code: tracing is
 * mandatory. Every child is counted before it starts, and given this run's
 * tracing variables whatever environment its creator hands it, so a child
 * that goes untraced is caught too.
 */

// windows.h must precede detours.h: detours.h's architecture check tests
// _AMD64_/_X86_/etc., which windows.h defines from the compiler's _M_* macros -
// detours.h doesn't include windows.h itself. winternl.h (for
// OBJECT_ATTRIBUTES, IO_STATUS_BLOCK, NTSTATUS, used by the Nt* signatures)
// also needs it first.
#include <windows.h>

#include <winternl.h>

#include <detours.h>

#include "stringutil.hpp"
#include "tracestatus.hpp"
#include "tracingenvironment.hpp"

#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

// winternl.h supplies NTSTATUS and the object/IO-status types but not this
// classic success predicate; define it only if some other header has not.
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace {

using makebelieve::StringUtil;
using makebelieve::TraceStatus;
using makebelieve::TracingEnvironment;

// The run's status block, mapped for the life of the process; null if it
// could not be opened, in which case this process cannot report at all - and
// is ended before it runs.
TraceStatus* g_status = nullptr;

// The run's failure event, signalled as a failure is reported; null only
// where g_status is too.
HANDLE g_failed = nullptr;

// What a process that cannot be traced exits with: the status the loader gives
// a process whose DLL failed to initialize.
constexpr UINT k_untraceable_exit = 0xC0000142;  // STATUS_DLL_INIT_FAILED

// Whether the detours were applied in this process, and so must be removed.
bool g_attached = false;

// This run's tracing variables, as this process was given them, which every
// process it starts is given in turn.
std::wstring g_log_path;
std::wstring g_root;
std::wstring g_status_name;
std::wstring g_test_failure;

std::wstring g_root_prefix_lower;  // lowercased, with a trailing separator
std::string g_hook_dll_utf8;       // as Detours wants it

// Set while this thread is inside record_if_interesting, so the file opens our
// own bookkeeping does (writing the log, and GetFinalPathNameByHandleW's own
// resolution) re-enter the Nt hooks without being recorded. The command's real
// open is never suppressed: the hooks always call the true Nt* function first,
// and only the recording that follows is guarded.
thread_local bool g_recording = false;

// The ntdll entry points we patch, resolved by name at attach time. Detours
// overwrites these with trampolines to the originals.
using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE,
                                        ACCESS_MASK,
                                        POBJECT_ATTRIBUTES,
                                        PIO_STATUS_BLOCK,
                                        PLARGE_INTEGER,
                                        ULONG,
                                        ULONG,
                                        ULONG,
                                        ULONG,
                                        PVOID,
                                        ULONG);
using NtOpenFileFn = NTSTATUS(NTAPI*)(PHANDLE,
                                      ACCESS_MASK,
                                      POBJECT_ATTRIBUTES,
                                      PIO_STATUS_BLOCK,
                                      ULONG,
                                      ULONG);

NtCreateFileFn TrueNtCreateFile = nullptr;
NtOpenFileFn TrueNtOpenFile = nullptr;

decltype(&::CreateProcessW) TrueCreateProcessW = ::CreateProcessW;
decltype(&::CreateProcessA) TrueCreateProcessA = ::CreateProcessA;

std::wstring to_lower(std::wstring_view s) {
  std::wstring result(s);
  for (wchar_t& c : result) {
    c = static_cast<wchar_t>(::towlower(c));
  }
  return result;
}

// Tells the run that a dependency may have been lost, and wakes it to end the
// command.
void report_failure() {
  if (g_status != nullptr) {
    g_status->report_failure();
  }
  if (g_failed != nullptr) {
    ::SetEvent(g_failed);
  }
}

/// Appends @a path to the log, reporting a failure - a lost dependency - if it
/// cannot.
///
/// Opening the log reaches the now-hooked NtCreateFile, but this only runs from
/// inside record_if_interesting where g_recording is set, so the re-entry skips
/// recording rather than looping.
void append_log_line(std::wstring_view path) {
  const HANDLE h = ::CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    report_failure();
    return;
  }
  std::string line = StringUtil::to_utf8(path);
  line.push_back('\n');
  DWORD written = 0;
  if (!::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written,
                   nullptr) ||
      written != line.size()) {
    report_failure();
  }
  ::CloseHandle(h);
}

/// Records the file behind @a handle if it was opened with read (data) access,
/// is a real non-directory file, and falls under the traced root. The path is
/// taken from the opened @a handle via GetFinalPathNameByHandleW, which
/// canonicalizes it (resolving relative or RootDirectory-based opens, and
/// symlinks) and avoids parsing ntdll's `\??\`-prefixed NT paths.
///
/// The read test looks for FILE_READ_DATA: kernel32 maps CreateFileW's
/// GENERIC_READ onto the specific FILE_GENERIC_READ rights before NtCreateFile,
/// so the generic bit is usually gone by here. GENERIC_READ is checked too, for
/// a caller that hands it straight to NtCreateFile.
void record_if_interesting(ACCESS_MASK desired_access, HANDLE handle) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      (desired_access & (FILE_READ_DATA | GENERIC_READ)) == 0 ||
      g_root_prefix_lower.empty()) {
    return;
  }

  BY_HANDLE_FILE_INFORMATION info{};
  if (!::GetFileInformationByHandle(handle, &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return;
  }

  wchar_t full[32768];
  const DWORD full_len = ::GetFinalPathNameByHandleW(
      handle, full, static_cast<DWORD>(std::size(full)),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (full_len == 0 || full_len >= std::size(full)) {
    return;
  }

  std::wstring_view resolved(full, full_len);
  // GetFinalPathNameByHandleW returns an extended-length ("\\?\") path; strip
  // that prefix so the report and the root check use ordinary DOS paths. A UNC
  // result ("\\?\UNC\...") keeps its remainder and simply won't match a
  // drive-letter root, which is correct - it is outside it.
  constexpr std::wstring_view k_extended_prefix = L"\\\\?\\";
  if (resolved.starts_with(k_extended_prefix)) {
    resolved.remove_prefix(k_extended_prefix.size());
  }

  const std::wstring lower = to_lower(resolved);
  if (lower.size() <= g_root_prefix_lower.size() ||
      lower.compare(0, g_root_prefix_lower.size(), g_root_prefix_lower) != 0) {
    return;  // outside the traced root
  }

  append_log_line(resolved);
}

/// Shared tail of both file hooks: on a successful open, record the file behind
/// the out-handle. Guarded by g_recording so the recording's own opens do not
/// re-enter it.
void maybe_record(NTSTATUS status, PHANDLE file_handle, ACCESS_MASK access) {
  if (g_recording || !NT_SUCCESS(status) || file_handle == nullptr) {
    return;
  }
  g_recording = true;
  record_if_interesting(access, *file_handle);
  g_recording = false;
}

NTSTATUS NTAPI HookedNtCreateFile(PHANDLE file_handle,
                                  ACCESS_MASK desired_access,
                                  POBJECT_ATTRIBUTES object_attributes,
                                  PIO_STATUS_BLOCK io_status_block,
                                  PLARGE_INTEGER allocation_size,
                                  ULONG file_attributes,
                                  ULONG share_access,
                                  ULONG create_disposition,
                                  ULONG create_options,
                                  PVOID ea_buffer,
                                  ULONG ea_length) {
  const NTSTATUS status = TrueNtCreateFile(
      file_handle, desired_access, object_attributes, io_status_block,
      allocation_size, file_attributes, share_access, create_disposition,
      create_options, ea_buffer, ea_length);
  maybe_record(status, file_handle, desired_access);
  return status;
}

NTSTATUS NTAPI HookedNtOpenFile(PHANDLE file_handle,
                                ACCESS_MASK desired_access,
                                POBJECT_ATTRIBUTES object_attributes,
                                PIO_STATUS_BLOCK io_status_block,
                                ULONG share_access,
                                ULONG open_options) {
  const NTSTATUS status =
      TrueNtOpenFile(file_handle, desired_access, object_attributes,
                     io_status_block, share_access, open_options);
  maybe_record(status, file_handle, desired_access);
  return status;
}

/// Resolves the ntdll file-open stubs we detour by name. GetModuleHandle/
/// GetProcAddress on already-mapped ntdll avoids a link-time dependency on an
/// import library the SDK does not uniformly provide.
void resolve_ntdll_targets() {
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return;
  }
  TrueNtCreateFile =
      reinterpret_cast<NtCreateFileFn>(::GetProcAddress(ntdll, "NtCreateFile"));
  TrueNtOpenFile =
      reinterpret_cast<NtOpenFileFn>(::GetProcAddress(ntdll, "NtOpenFile"));
}

// The UTF-16 environment block a child starts with: @a environment as its
// creator gave it - UTF-16 if @a unicode - or, if null, this process's own,
// with this run's tracing variables in place of any it carried, in whatever
// case. So a child is traced whatever environment it is handed, and whatever
// this process has done to its own.
std::wstring child_environment(LPVOID environment, bool unicode) {
  std::vector<std::wstring> entries =
      environment == nullptr ? TracingEnvironment::own_entries()
      : unicode              ? TracingEnvironment::entries_of(
                      static_cast<const wchar_t*>(environment))
                : TracingEnvironment::entries_of(
                      static_cast<const char*>(environment));
  std::vector<TracingEnvironment::Variable> tracing = {
      {TraceStatus::k_log_var, g_log_path},
      {TraceStatus::k_root_var, g_root},
      {TraceStatus::k_status_var, g_status_name}};
  if (!g_test_failure.empty()) {
    tracing.emplace_back(
        TraceStatus::k_test_failure_var,
        g_test_failure == TraceStatus::k_fail_absent_in_children
            ? std::wstring_view(TraceStatus::k_fail_absent)
            : std::wstring_view(g_test_failure));
  }
  return TracingEnvironment::build(std::move(entries), tracing);
}

// Starts a child through @a create - given the flags and environment to use -
// counted as expected before it can acknowledge its hooks, and uncounted if it
// could not be started.
template <class Create>
BOOL start_child(DWORD creation_flags, LPVOID environment, Create create) {
  std::wstring block;
  try {
    block = child_environment(
        environment, (creation_flags & CREATE_UNICODE_ENVIRONMENT) != 0);
  } catch (...) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }
  g_status->expect_process();
  const BOOL created =
      create(creation_flags | CREATE_UNICODE_ENVIRONMENT, block.data());
  if (!created) {
    const DWORD error = ::GetLastError();
    g_status->unexpect_process();
    ::SetLastError(error);
  }
  return created;
}

/// Re-injects this DLL into a newly-created child by forwarding to
/// DetourCreateProcessWithDllEx, using its pfCreateProcessW/A extensibility
/// point.
BOOL WINAPI HookedCreateProcessW(LPCWSTR application_name,
                                 LPWSTR command_line,
                                 LPSECURITY_ATTRIBUTES process_attributes,
                                 LPSECURITY_ATTRIBUTES thread_attributes,
                                 BOOL inherit_handles,
                                 DWORD creation_flags,
                                 LPVOID environment,
                                 LPCWSTR current_directory,
                                 LPSTARTUPINFOW startup_info,
                                 LPPROCESS_INFORMATION process_information) {
  return start_child(creation_flags, environment,
                     [&](DWORD flags, LPVOID block) {
                       return ::DetourCreateProcessWithDllExW(
                           application_name, command_line, process_attributes,
                           thread_attributes, inherit_handles, flags, block,
                           current_directory, startup_info, process_information,
                           g_hook_dll_utf8.c_str(), TrueCreateProcessW);
                     });
}

BOOL WINAPI HookedCreateProcessA(LPCSTR application_name,
                                 LPSTR command_line,
                                 LPSECURITY_ATTRIBUTES process_attributes,
                                 LPSECURITY_ATTRIBUTES thread_attributes,
                                 BOOL inherit_handles,
                                 DWORD creation_flags,
                                 LPVOID environment,
                                 LPCSTR current_directory,
                                 LPSTARTUPINFOA startup_info,
                                 LPPROCESS_INFORMATION process_information) {
  return start_child(creation_flags, environment,
                     [&](DWORD flags, LPVOID block) {
                       return ::DetourCreateProcessWithDllExA(
                           application_name, command_line, process_attributes,
                           thread_attributes, inherit_handles, flags, block,
                           current_directory, startup_info, process_information,
                           g_hook_dll_utf8.c_str(), TrueCreateProcessA);
                     });
}

// The environment variable @a name, or nothing if it is unset or too long.
std::wstring environment_variable(const wchar_t* name) {
  std::vector<wchar_t> buffer(32768);
  const DWORD n = ::GetEnvironmentVariableW(name, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
  if (n == 0 || n >= buffer.size()) {
    return {};
  }
  return {buffer.data(), n};
}

// Maps the run's status block, named by g_status_name, into g_status, and
// opens its failure event.
void open_status() {
  if (g_status_name.empty()) {
    return;
  }
  // Kept open for the life of the process.
  g_failed = ::OpenEventW(
      EVENT_MODIFY_STATE, FALSE,
      (g_status_name + TraceStatus::k_failure_event_suffix).c_str());
  if (g_failed == nullptr) {
    return;
  }
  const HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE,
                                            FALSE, g_status_name.c_str());
  if (mapping == nullptr) {
    return;
  }
  // The view keeps the section alive, and stays mapped until the process
  // exits.
  g_status = static_cast<TraceStatus*>(::MapViewOfFile(
      mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(TraceStatus)));
  ::CloseHandle(mapping);
}

// Reads this run's tracing variables and this DLL's own path, and opens the
// status block; false if anything needed to trace is missing.
bool read_config(HINSTANCE hinst) {
  g_log_path = environment_variable(TraceStatus::k_log_var);
  g_root = environment_variable(TraceStatus::k_root_var);
  g_status_name = environment_variable(TraceStatus::k_status_var);
  if (!g_root.empty()) {
    std::wstring root = g_root;
    if (root.back() != L'\\') {
      root.push_back(L'\\');
    }
    g_root_prefix_lower = to_lower(root);
  }
  std::vector<wchar_t> module(32768);
  const DWORD module_len = ::GetModuleFileNameW(
      hinst, module.data(), static_cast<DWORD>(module.size()));
  if (module_len > 0 && module_len < module.size()) {
    g_hook_dll_utf8 =
        StringUtil::to_utf8(std::wstring_view(module.data(), module_len));
  }
  open_status();
  return g_status != nullptr && !g_log_path.empty() && !g_root.empty() &&
         !g_hook_dll_utf8.empty();
}

// Installs the detours, or says it could not.
bool install() {
  resolve_ntdll_targets();
  if (TrueNtCreateFile == nullptr || TrueNtOpenFile == nullptr) {
    return false;
  }
  // DetourAttach records any failure in the transaction, and Commit then
  // reports it, having applied none of the detours.
  ::DetourTransactionBegin();
  ::DetourUpdateThread(::GetCurrentThread());
  ::DetourAttach(&(PVOID&)TrueNtCreateFile, HookedNtCreateFile);
  ::DetourAttach(&(PVOID&)TrueNtOpenFile, HookedNtOpenFile);
  ::DetourAttach(&(PVOID&)TrueCreateProcessW, HookedCreateProcessW);
  ::DetourAttach(&(PVOID&)TrueCreateProcessA, HookedCreateProcessA);
  return ::DetourTransactionCommit() == NO_ERROR;
}

// Ends a process that cannot be traced before it runs any code of its own -
// this runs within the loader, before its entry point - having told the run
// why where it can. Terminated rather than failing the load, which would have
// the loader raise a hard-error dialog that nobody is there to dismiss.
void refuse_to_run() {
  report_failure();
  ::TerminateProcess(::GetCurrentProcess(), k_untraceable_exit);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID /*reserved*/) {
  if (::DetourIsHelperProcess()) {
    return TRUE;
  }

  if (reason == DLL_PROCESS_ATTACH) {
    ::DetourRestoreAfterWith();
    ::DisableThreadLibraryCalls(hinst);
    g_test_failure = environment_variable(TraceStatus::k_test_failure_var);
    if (g_test_failure == TraceStatus::k_fail_absent) {
      return TRUE;
    }
    bool ready = false;
    try {
      ready = read_config(hinst) &&
              g_test_failure != TraceStatus::k_fail_install && install();
    } catch (...) {
      ready = false;
    }
    if (!ready) {
      refuse_to_run();
      return FALSE;
    }
    g_attached = true;
    g_status->report_installed();
  } else if (reason == DLL_PROCESS_DETACH) {
    if (!g_attached) {
      return TRUE;
    }
    ::DetourTransactionBegin();
    ::DetourUpdateThread(::GetCurrentThread());
    ::DetourDetach(&(PVOID&)TrueNtCreateFile, HookedNtCreateFile);
    ::DetourDetach(&(PVOID&)TrueNtOpenFile, HookedNtOpenFile);
    ::DetourDetach(&(PVOID&)TrueCreateProcessW, HookedCreateProcessW);
    ::DetourDetach(&(PVOID&)TrueCreateProcessA, HookedCreateProcessA);
    ::DetourTransactionCommit();
  }
  return TRUE;
}
