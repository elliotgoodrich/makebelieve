// SPDX-License-Identifier: MIT
/**
 * @file
 * The payload DLL makebelieve injects into a build command, and transitively
 * every child process it spawns, to discover what under the traced root the
 * command looked at (the log path and root come from environment variables).
 * It detours these ntdll stubs, which every user-mode lookup, stat and listing
 * funnels through, so hooking here sees them all, and CreateProcessW/A:
 *
 *   - NtCreateFile/NtOpenFile: records the resolved path of every open that
 *     reads, or asks only for attributes, of a file or directory - and the
 *     path asked for when it finds nothing there.
 *   - NtQueryAttributesFile/NtQueryFullAttributesFile: a stat by path, with
 *     no handle; recorded as such an open would be.
 *   - NtQueryDirectoryFile/NtQueryDirectoryFileEx: records a listing of the
 *     directory, marked as one - by names alone, or with each entry's
 *     attributes, as the information class asks - or, for a query of one
 *     name, as FindFirstFile makes on a path, a lookup of that name.
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

#include "scopeguard.hpp"
#include "stringutil.hpp"
#include "tracestatus.hpp"
#include "tracingenvironment.hpp"

#include <cstddef>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// winternl.h supplies NTSTATUS and the object/IO-status types but not this
// classic success predicate; define it only if some other header has not.
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

using namespace makebelieve;

namespace {

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

// The root as a handle's final path spells it, with a trailing separator, and
// that lowercased: what the paths of everything under it start with. The same
// as g_root's if the root cannot be opened.
std::wstring g_root_final_prefix;
std::wstring g_root_final_prefix_lower;
std::string g_hook_dll_utf8;  // as Detours wants it

// Set while this thread is recording, so the file opens our
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

// Stats by path, and listings. The information structures are left untyped,
// as the hook never looks inside them.
using NtQueryAttributesFileFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
using NtQueryDirectoryFileFn = NTSTATUS(NTAPI*)(HANDLE,
                                                HANDLE,
                                                PIO_APC_ROUTINE,
                                                PVOID,
                                                PIO_STATUS_BLOCK,
                                                PVOID,
                                                ULONG,
                                                ULONG,
                                                BOOLEAN,
                                                PUNICODE_STRING,
                                                BOOLEAN);
using NtQueryDirectoryFileExFn = NTSTATUS(NTAPI*)(HANDLE,
                                                  HANDLE,
                                                  PIO_APC_ROUTINE,
                                                  PVOID,
                                                  PIO_STATUS_BLOCK,
                                                  PVOID,
                                                  ULONG,
                                                  ULONG,
                                                  ULONG,
                                                  PUNICODE_STRING);

NtCreateFileFn TrueNtCreateFile = nullptr;
NtOpenFileFn TrueNtOpenFile = nullptr;
NtQueryAttributesFileFn TrueNtQueryAttributesFile = nullptr;
NtQueryAttributesFileFn TrueNtQueryFullAttributesFile = nullptr;
NtQueryDirectoryFileFn TrueNtQueryDirectoryFile = nullptr;
NtQueryDirectoryFileExFn TrueNtQueryDirectoryFileEx = nullptr;

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

/// Appends @a text, and a newline, to the log, reporting a failure - a lost
/// dependency - if it cannot.
///
/// Opening the log reaches the now-hooked NtCreateFile, but this only runs from
/// inside a recording, where g_recording is set, so the re-entry skips
/// recording rather than looping.
void append_log_line(std::wstring_view text) {
  const HANDLE h = ::CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    report_failure();
    return;
  }
  const ScopeGuard close_log([h] { ::CloseHandle(h); });
  std::string line = StringUtil::to_utf8(text);
  line.push_back('\n');
  DWORD written = 0;
  if (!::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written,
                   nullptr) ||
      written != line.size()) {
    report_failure();
  }
}

// What this process has logged already, so a command that looks at one file a
// thousand times logs it once. Guarded by g_logged_lock.
std::unordered_set<std::wstring>* g_logged = nullptr;
SRWLOCK g_logged_lock = SRWLOCK_INIT;

/// Logs, once, that the command looked at @a path, under the traced root -
/// looked it up, whether it found anything there or not - or, if @a listed,
/// listed it, which the log marks as TraceStatus::k_listed_marker says.
void log_look(std::wstring_view path, bool listed = false) {
  std::wstring key;
  key.reserve(path.size() + 1);
  if (listed) {
    key.push_back(static_cast<wchar_t>(TraceStatus::k_listed_marker));
  }
  key += path;
  ::AcquireSRWLockExclusive(&g_logged_lock);
  const ScopeGuard release([] { ::ReleaseSRWLockExclusive(&g_logged_lock); });
  if (g_logged == nullptr) {
    // Never freed: a process's last hooked calls may come during its exit.
    g_logged = new std::unordered_set<std::wstring>();
  }
  if (!g_logged->insert(key).second) {
    return;
  }
  append_log_line(key);
}

constexpr std::wstring_view k_extended_prefix = L"\\\\?\\";

/// The path of what @a handle has open, normalized, as an ordinary DOS path,
/// or nothing if it has none. GetFinalPathNameByHandleW canonicalizes it
/// (resolving relative or RootDirectory-based opens, and symlinks) and avoids
/// parsing ntdll's `\??\`-prefixed NT paths. Its extended-length (`\\?\`)
/// prefix is dropped, so a UNC result (`\\?\UNC\...`) keeps its remainder and
/// simply won't match a drive-letter root, which is correct - it is outside.
std::optional<std::wstring> final_path(HANDLE handle) {
  std::vector<wchar_t> full(32768);
  const DWORD full_len = ::GetFinalPathNameByHandleW(
      handle, full.data(), static_cast<DWORD>(full.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (full_len == 0 || full_len >= full.size()) {
    return std::nullopt;
  }
  std::wstring_view resolved(full.data(), full_len);
  if (resolved.starts_with(k_extended_prefix)) {
    resolved.remove_prefix(k_extended_prefix.size());
  }
  return std::wstring(resolved);
}

/// The part of @a path beneath the root @a prefix_lower - lowercased, with a
/// trailing separator - without separators at either end, or nothing if it
/// is not beneath it. The root itself, which a command always finds there, is
/// not beneath it.
std::optional<std::wstring_view> beneath(std::wstring_view path,
                                         const std::wstring& prefix_lower) {
  if (prefix_lower.empty() || path.size() <= prefix_lower.size() ||
      to_lower(path.substr(0, prefix_lower.size())) != prefix_lower) {
    return std::nullopt;
  }
  std::wstring_view rest = path.substr(prefix_lower.size());
  while (!rest.empty() && rest.back() == L'\\') {
    rest.remove_suffix(1);
  }
  if (rest.empty()) {
    return std::nullopt;
  }
  return rest;
}

/// Logs what @a handle has open if it falls under the traced root.
void log_handle(HANDLE handle) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return;
  }
  const std::optional<std::wstring> path = final_path(handle);
  if (path.has_value() && beneath(*path, g_root_final_prefix_lower)) {
    log_look(*path);
  }
}

/// The path @a attributes names, as an ordinary DOS path spelled as the
/// caller spelled it - not canonicalized, since there may be nothing there to
/// canonicalize it against - or nothing if it cannot be told.
std::optional<std::wstring> named_path(const OBJECT_ATTRIBUTES* attributes) {
  if (attributes == nullptr || attributes->ObjectName == nullptr ||
      attributes->ObjectName->Buffer == nullptr) {
    return std::nullopt;
  }
  std::wstring_view name(attributes->ObjectName->Buffer,
                         attributes->ObjectName->Length / sizeof(wchar_t));
  if (attributes->RootDirectory != nullptr) {
    std::optional<std::wstring> directory =
        final_path(attributes->RootDirectory);
    if (directory.has_value() && !name.empty()) {
      if (!directory->ends_with(L'\\')) {
        directory->push_back(L'\\');
      }
      *directory += name;
    }
    return directory;
  }
  for (const std::wstring_view prefix :
       {std::wstring_view(L"\\??\\"), k_extended_prefix}) {
    if (name.starts_with(prefix)) {
      name.remove_prefix(prefix.size());
      break;
    }
  }
  // Only a drive-letter path can be under the root.
  if (name.size() < 3 || name[1] != L':' || name[2] != L'\\') {
    return std::nullopt;
  }
  return std::wstring(name);
}

/// @a path, as named_path() gives it, beneath the root as the root's final
/// path spells it - so it matches what a handle's final path would be, but
/// for the case and form of the rest - or nothing if it is not beneath it.
std::optional<std::wstring> rooted(std::wstring_view path) {
  for (const std::wstring* prefix :
       {&g_root_final_prefix_lower, &g_root_prefix_lower}) {
    if (const std::optional<std::wstring_view> rest = beneath(path, *prefix)) {
      return g_root_final_prefix + std::wstring(*rest);
    }
  }
  return std::nullopt;
}

/// Logs the path @a attributes names as looked up, if it is under the root:
/// as found, by the final path of what is there, if @a found; or as the
/// caller spelled it, if not.
void log_named(const OBJECT_ATTRIBUTES* attributes, bool found) {
  const std::optional<std::wstring> path = named_path(attributes);
  if (!path.has_value()) {
    return;
  }
  const std::optional<std::wstring> under = rooted(*path);
  if (!under.has_value()) {
    return;  // outside the traced root
  }
  if (found) {
    // Opened afresh, as a stat would see it, for the spelling the directory
    // gives it, which a change to it is told by. The real open has already
    // happened, so this one never changes what the command sees.
    HANDLE handle = nullptr;
    IO_STATUS_BLOCK status_block{};
    OBJECT_ATTRIBUTES copy = *attributes;
    if (NT_SUCCESS(TrueNtOpenFile(
            &handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &copy, &status_block,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT))) {
      const ScopeGuard close([handle] { ::CloseHandle(handle); });
      const std::optional<std::wstring> final = final_path(handle);
      if (final.has_value() && beneath(*final, g_root_final_prefix_lower)) {
        log_look(*final);
        return;
      }
    }
  }
  log_look(*under);
}

// What an NTSTATUS says of a lookup that found nothing there.
constexpr NTSTATUS k_status_object_name_not_found =
    static_cast<NTSTATUS>(0xC0000034L);
constexpr NTSTATUS k_status_object_path_not_found =
    static_cast<NTSTATUS>(0xC000003AL);
constexpr NTSTATUS k_status_no_such_file = static_cast<NTSTATUS>(0xC000000FL);
constexpr NTSTATUS k_status_no_more_files = static_cast<NTSTATUS>(0x80000006L);
constexpr NTSTATUS k_status_pending = static_cast<NTSTATUS>(0x00000103L);

bool is_not_found(NTSTATUS status) {
  return status == k_status_object_name_not_found ||
         status == k_status_object_path_not_found;
}

/// Whether an open with @a desired_access looks at what it opens: reads its
/// data, or - asking for nothing more than its attributes, as a stat does -
/// looks at those. kernel32 maps CreateFileW's GENERIC_READ onto the
/// specific FILE_GENERIC_READ rights before NtCreateFile, so the generic bit
/// is usually gone by here; it is checked too, for a caller that hands it
/// straight to NtCreateFile. kernel32 also adds FILE_READ_ATTRIBUTES to every
/// open, a write's included, which is why it counts only on its own.
bool looks_at(ACCESS_MASK desired_access) {
  if ((desired_access & (FILE_READ_DATA | GENERIC_READ)) != 0) {
    return true;
  }
  constexpr ACCESS_MASK k_harmless =
      FILE_READ_ATTRIBUTES | FILE_READ_EA | READ_CONTROL | SYNCHRONIZE;
  return (desired_access & FILE_READ_ATTRIBUTES) != 0 &&
         (desired_access & ~k_harmless) == 0;
}

/// Shared tail of both file hooks: an open that looks at what it opens, and
/// finds it, records what it opened - a file or a directory - by its final
/// path; one that finds nothing there records the path it asked for. Guarded
/// by g_recording so the recording's own opens do not re-enter it.
void maybe_record(NTSTATUS status,
                  PHANDLE file_handle,
                  ACCESS_MASK access,
                  const OBJECT_ATTRIBUTES* attributes) {
  if (g_recording || g_root_prefix_lower.empty() || !looks_at(access)) {
    return;
  }
  const bool found = NT_SUCCESS(status) && file_handle != nullptr;
  if (!found && !is_not_found(status)) {
    return;
  }
  g_recording = true;
  if (found) {
    log_handle(*file_handle);
  } else {
    log_named(attributes, false);
  }
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
  maybe_record(status, file_handle, desired_access, object_attributes);
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
  maybe_record(status, file_handle, desired_access, object_attributes);
  return status;
}

/// Shared tail of both attribute queries - a stat by path, with no handle -
/// which look up the path they name, whether they find it or not.
void record_query(NTSTATUS status, const OBJECT_ATTRIBUTES* attributes) {
  if (g_recording || g_root_prefix_lower.empty()) {
    return;
  }
  const bool found = NT_SUCCESS(status);
  if (!found && !is_not_found(status)) {
    return;
  }
  g_recording = true;
  log_named(attributes, found);
  g_recording = false;
}

NTSTATUS NTAPI HookedNtQueryAttributesFile(POBJECT_ATTRIBUTES object_attributes,
                                           PVOID file_information) {
  const NTSTATUS status =
      TrueNtQueryAttributesFile(object_attributes, file_information);
  record_query(status, object_attributes);
  return status;
}

NTSTATUS NTAPI
HookedNtQueryFullAttributesFile(POBJECT_ATTRIBUTES object_attributes,
                                PVOID file_information) {
  const NTSTATUS status =
      TrueNtQueryFullAttributesFile(object_attributes, file_information);
  record_query(status, object_attributes);
  return status;
}

/// Whether @a pattern, as a directory query's FileName, could match more than
/// one name: it holds a wildcard, as the filesystem's own matching counts
/// them - DOS_STAR, DOS_QM and DOS_DOT included.
bool is_wildcard(std::wstring_view pattern) {
  return pattern.find_first_of(L"*?<>\"") != std::wstring_view::npos;
}

/// Shared tail of both directory queries. A query for one name - FindFirstFile
/// on a path, as a stat - looks that name up; anything else lists the
/// directory, wholly, since what a narrower pattern sees changes only with
/// it. A listing is recorded whatever it returned, and before it may have
/// finished, as it depends on the directory either way. Whatever else it
/// returns of each entry - its size and times - is not depended on: a
/// directory's value is its entries' names, as on Linux.
void record_directory_query(NTSTATUS status,
                            HANDLE directory,
                            const UNICODE_STRING* file_name) {
  if (g_recording || g_root_prefix_lower.empty()) {
    return;
  }
  if (!NT_SUCCESS(status) && status != k_status_pending &&
      status != k_status_no_more_files && status != k_status_no_such_file) {
    return;
  }
  g_recording = true;
  const std::wstring_view pattern =
      file_name != nullptr && file_name->Buffer != nullptr
          ? std::wstring_view(file_name->Buffer,
                              file_name->Length / sizeof(wchar_t))
          : std::wstring_view();
  if (std::optional<std::wstring> path = final_path(directory)) {
    if (!pattern.empty() && !is_wildcard(pattern)) {
      if (!path->ends_with(L'\\')) {
        path->push_back(L'\\');
      }
      *path += pattern;
      if (beneath(*path, g_root_final_prefix_lower)) {
        log_look(*path);
      }
    } else if (to_lower(*path) + L'\\' == g_root_final_prefix_lower ||
               beneath(*path, g_root_final_prefix_lower)) {
      log_look(*path, true);
    }
  }
  g_recording = false;
}

NTSTATUS NTAPI HookedNtQueryDirectoryFile(HANDLE file_handle,
                                          HANDLE event,
                                          PIO_APC_ROUTINE apc_routine,
                                          PVOID apc_context,
                                          PIO_STATUS_BLOCK io_status_block,
                                          PVOID file_information,
                                          ULONG length,
                                          ULONG file_information_class,
                                          BOOLEAN return_single_entry,
                                          PUNICODE_STRING file_name,
                                          BOOLEAN restart_scan) {
  const NTSTATUS status = TrueNtQueryDirectoryFile(
      file_handle, event, apc_routine, apc_context, io_status_block,
      file_information, length, file_information_class, return_single_entry,
      file_name, restart_scan);
  record_directory_query(status, file_handle, file_name);
  return status;
}

NTSTATUS NTAPI HookedNtQueryDirectoryFileEx(HANDLE file_handle,
                                            HANDLE event,
                                            PIO_APC_ROUTINE apc_routine,
                                            PVOID apc_context,
                                            PIO_STATUS_BLOCK io_status_block,
                                            PVOID file_information,
                                            ULONG length,
                                            ULONG file_information_class,
                                            ULONG query_flags,
                                            PUNICODE_STRING file_name) {
  const NTSTATUS status = TrueNtQueryDirectoryFileEx(
      file_handle, event, apc_routine, apc_context, io_status_block,
      file_information, length, file_information_class, query_flags, file_name);
  record_directory_query(status, file_handle, file_name);
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
  TrueNtQueryAttributesFile = reinterpret_cast<NtQueryAttributesFileFn>(
      ::GetProcAddress(ntdll, "NtQueryAttributesFile"));
  TrueNtQueryFullAttributesFile = reinterpret_cast<NtQueryAttributesFileFn>(
      ::GetProcAddress(ntdll, "NtQueryFullAttributesFile"));
  TrueNtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFileFn>(
      ::GetProcAddress(ntdll, "NtQueryDirectoryFile"));
  // Only from Windows 10 1709; where it is missing, nothing can call it.
  TrueNtQueryDirectoryFileEx = reinterpret_cast<NtQueryDirectoryFileExFn>(
      ::GetProcAddress(ntdll, "NtQueryDirectoryFileEx"));
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
    g_root_final_prefix = root;
    // Before the detours are in, so nothing here is recorded.
    const HANDLE handle = ::CreateFileW(
        g_root.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      if (std::optional<std::wstring> final = final_path(handle)) {
        if (!final->ends_with(L'\\')) {
          final->push_back(L'\\');
        }
        g_root_final_prefix = std::move(*final);
      }
      ::CloseHandle(handle);
    }
    g_root_final_prefix_lower = to_lower(g_root_final_prefix);
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
  if (TrueNtCreateFile == nullptr || TrueNtOpenFile == nullptr ||
      TrueNtQueryAttributesFile == nullptr ||
      TrueNtQueryFullAttributesFile == nullptr ||
      TrueNtQueryDirectoryFile == nullptr) {
    return false;
  }
  // DetourAttach records any failure in the transaction, and Commit then
  // reports it, having applied none of the detours.
  ::DetourTransactionBegin();
  ::DetourUpdateThread(::GetCurrentThread());
  ::DetourAttach(&(PVOID&)TrueNtCreateFile, HookedNtCreateFile);
  ::DetourAttach(&(PVOID&)TrueNtOpenFile, HookedNtOpenFile);
  ::DetourAttach(&(PVOID&)TrueNtQueryAttributesFile,
                 HookedNtQueryAttributesFile);
  ::DetourAttach(&(PVOID&)TrueNtQueryFullAttributesFile,
                 HookedNtQueryFullAttributesFile);
  ::DetourAttach(&(PVOID&)TrueNtQueryDirectoryFile, HookedNtQueryDirectoryFile);
  if (TrueNtQueryDirectoryFileEx != nullptr) {
    ::DetourAttach(&(PVOID&)TrueNtQueryDirectoryFileEx,
                   HookedNtQueryDirectoryFileEx);
  }
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
    ::DetourDetach(&(PVOID&)TrueNtQueryAttributesFile,
                   HookedNtQueryAttributesFile);
    ::DetourDetach(&(PVOID&)TrueNtQueryFullAttributesFile,
                   HookedNtQueryFullAttributesFile);
    ::DetourDetach(&(PVOID&)TrueNtQueryDirectoryFile,
                   HookedNtQueryDirectoryFile);
    if (TrueNtQueryDirectoryFileEx != nullptr) {
      ::DetourDetach(&(PVOID&)TrueNtQueryDirectoryFileEx,
                     HookedNtQueryDirectoryFileEx);
    }
    ::DetourDetach(&(PVOID&)TrueCreateProcessW, HookedCreateProcessW);
    ::DetourDetach(&(PVOID&)TrueCreateProcessA, HookedCreateProcessA);
    ::DetourTransactionCommit();
  }
  return TRUE;
}
