/**
 * @file
 * Payload DLL injected (via DetourCreateProcessWithDllExW - see
 * detours_tracing.m.cpp) into the traced command and, transitively, every
 * child process it spawns. Detours NtCreateFile/NtOpenFile and
 * CreateProcessW/CreateProcessA:
 *
 *   - NtCreateFile/NtOpenFile: records the resolved path of every
 *     successful, read-access, non-directory open that falls under the
 *     traced root (both passed in via environment variables the launcher
 *     sets before spawning), appending it to a shared log file. These are
 *     the two ntdll stubs *every* user-mode file open funnels through -
 *     whether the caller reached them via CreateFileW/A, the C runtime
 *     (fopen/ifstream/...), or by calling NtCreateFile directly - so
 *     hooking here sees all of them, unlike hooking CreateFileW/A which
 *     only sees opens made through those two Win32 entry points.
 *   - CreateProcessW/A: re-injects this same DLL into every child process,
 *     via the same DetourCreateProcessWithDllEx mechanism the launcher
 *     itself uses, so descendant processes are traced too - something
 *     FUSE_tracing gets for free from being mount-scoped rather than
 *     process-scoped (see ../ETW_tracing/README.md for the same point
 *     made about ETW, which gets it for free from being system-scoped
 *     instead). Process creation is left at the Win32 layer on purpose:
 *     Detours' re-injection is built around CreateProcess, not the
 *     NtCreateUserProcess stub beneath it.
 *
 * Findings are appended to a plain log file rather than streamed back
 * over a pipe: every write completes synchronously inside the hooked
 * call, so by the time the launcher's WaitForSingleObject(hProcess)
 * returns, every write this process - and any already-exited descendant -
 * ever made is already durably on disk; no separate flush or handshake is
 * needed before the launcher reads it back. Multiple processes can safely
 * append to the same file concurrently this way: a handle opened with
 * only FILE_APPEND_DATA gets atomic append semantics from the filesystem,
 * the same guarantee POSIX O_APPEND makes.
 */

// windows.h must precede detours.h: detours.h's architecture check (X86 vs
// AMD64 vs ...) tests _AMD64_/_X86_/etc., which windows.h's own headers
// define from the compiler's _M_* macros - detours.h doesn't include
// windows.h itself, so without this order it sees none of them defined
// and fails with "Unknown architecture". winternl.h (for OBJECT_ATTRIBUTES,
// IO_STATUS_BLOCK, NTSTATUS and friends, used by the Nt* signatures below)
// likewise needs windows.h first.
#include <windows.h>

#include <winternl.h>

#include <detours.h>

#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>

// winternl.h supplies NTSTATUS and the object/IO-status types but not this
// classic success predicate; define it only if some other header has not.
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace {

constexpr wchar_t kLogEnvVar[] = L"MAKEBELIEVE_DETOURS_TRACING_LOG";
constexpr wchar_t kRootEnvVar[] = L"MAKEBELIEVE_DETOURS_TRACING_ROOT";

std::wstring g_log_path;
std::wstring g_root_prefix_lower;  // lowercased, with a trailing separator
std::wstring g_hook_dll_path;

// Set while this thread is inside record_if_interesting, so the file opens
// our own bookkeeping performs (writing the log line, and the volume/path
// resolution GetFinalPathNameByHandleW may itself do) re-enter the Nt hooks
// without being recorded - or recursing into recording again. The traced
// command's real open is never suppressed by this: the hooks always call
// the true Nt* function first and unconditionally, and only the recording
// that follows is guarded.
thread_local bool g_recording = false;

// The ntdll entry points we patch, resolved by name at attach time (see
// DllMain). Detours overwrites these with trampolines to the originals.
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

std::string to_utf8(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                        static_cast<int>(wide.size()), nullptr,
                                        0, nullptr, nullptr);
  if (len <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        result.data(), len, nullptr, nullptr);
  return result;
}

/// Best-effort: a failure here just means one dependency goes unreported,
/// consistent with this tracer's general best-effort posture (see the
/// file comment).
///
/// This opens the log through the ordinary CreateFileW, which reaches the
/// now-hooked NtCreateFile - but it only runs from inside
/// record_if_interesting, where g_recording is set, so that re-entry skips
/// recording rather than looping. (It would be harmless even without the
/// guard: the log is opened FILE_APPEND_DATA-only, which fails
/// record_if_interesting's read-access check immediately - but the guard
/// spares the whole detour round-trip on every logged access.)
void append_log_line(std::wstring_view path) {
  if (g_log_path.empty()) {
    return;
  }
  const HANDLE h = ::CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  std::string line = to_utf8(path);
  line.push_back('\n');
  DWORD written = 0;
  ::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written,
              nullptr);
  ::CloseHandle(h);
}

/// Records the file behind @a handle if it was opened with read (data)
/// access, is a real non-directory file, and falls under the traced root.
/// The path is taken from the successfully-opened @a handle itself via
/// GetFinalPathNameByHandleW rather than from the call's ObjectName: that
/// canonicalizes it (resolving a relative or RootDirectory-based open, and
/// following symlinks) and sidesteps parsing ntdll's `\??\`-prefixed NT
/// paths. GetFinalPathNameByHandleW is Vista+, which is why the build
/// raises _WIN32_WINNT above Detours' own Makefile default (see the
/// experiment's CMakeLists.txt).
///
/// The read test looks for FILE_READ_DATA, not GENERIC_READ: kernel32 maps
/// a CreateFileW(GENERIC_READ) request onto the specific FILE_GENERIC_READ
/// rights before it reaches NtCreateFile, so by this layer the generic bit
/// is usually already gone. GENERIC_READ is still checked as well, for a
/// caller that hands the generic bit straight to NtCreateFile.
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
  // GetFinalPathNameByHandleW returns an extended-length ("\\?\") path;
  // strip that prefix so the report and the root check use ordinary DOS
  // paths, the way the CreateFile-based tracer's GetFullPathNameW output
  // did. A UNC result ("\\?\UNC\...") keeps its remainder and simply won't
  // match a drive-letter root, which is correct - it is outside it.
  constexpr std::wstring_view k_extended_prefix = L"\\\\?\\";
  if (resolved.starts_with(k_extended_prefix)) {
    resolved.remove_prefix(k_extended_prefix.size());
  }

  const std::wstring lower = to_lower(resolved);
  if (lower.size() <= g_root_prefix_lower.size() ||
      lower.compare(0, g_root_prefix_lower.size(), g_root_prefix_lower) != 0) {
    return;  // outside the current directory - not tracked, same as ".."
  }

  append_log_line(resolved);
}

/// Shared tail of both file hooks: on a successful open, record the file
/// behind the out-handle. Guarded by g_recording so the recording's own
/// opens (see append_log_line, and GetFinalPathNameByHandleW's own volume
/// resolution) do not re-enter it.
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

/// Re-injects this DLL into a newly-created child process by forwarding
/// to DetourCreateProcessWithDllEx instead of the true CreateProcess -
/// exactly the extensibility point its own pfCreateProcessW/A parameter
/// exists for (it's what the real creation call is made through). Falls
/// back to the plain, un-injected create if the hook DLL's own path was
/// never resolved (see DllMain), rather than failing the caller's
/// CreateProcess call outright.
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
  if (g_hook_dll_path.empty()) {
    return TrueCreateProcessW(
        application_name, command_line, process_attributes, thread_attributes,
        inherit_handles, creation_flags, environment, current_directory,
        startup_info, process_information);
  }
  const std::string dll_utf8 = to_utf8(g_hook_dll_path);
  return ::DetourCreateProcessWithDllExW(
      application_name, command_line, process_attributes, thread_attributes,
      inherit_handles, creation_flags, environment, current_directory,
      startup_info, process_information, dll_utf8.c_str(), TrueCreateProcessW);
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
  if (g_hook_dll_path.empty()) {
    return TrueCreateProcessA(
        application_name, command_line, process_attributes, thread_attributes,
        inherit_handles, creation_flags, environment, current_directory,
        startup_info, process_information);
  }
  const std::string dll_utf8 = to_utf8(g_hook_dll_path);
  return ::DetourCreateProcessWithDllExA(
      application_name, command_line, process_attributes, thread_attributes,
      inherit_handles, creation_flags, environment, current_directory,
      startup_info, process_information, dll_utf8.c_str(), TrueCreateProcessA);
}

void read_env_config(HINSTANCE hinst) {
  wchar_t buffer[32768];

  DWORD n = ::GetEnvironmentVariableW(kLogEnvVar, buffer,
                                      static_cast<DWORD>(std::size(buffer)));
  if (n > 0 && n < std::size(buffer)) {
    g_log_path.assign(buffer, n);
  }

  n = ::GetEnvironmentVariableW(kRootEnvVar, buffer,
                                static_cast<DWORD>(std::size(buffer)));
  if (n > 0 && n < std::size(buffer)) {
    std::wstring root(buffer, n);
    if (!root.empty() && root.back() != L'\\') {
      root.push_back(L'\\');
    }
    g_root_prefix_lower = to_lower(root);
  }

  const DWORD module_len = ::GetModuleFileNameW(
      hinst, buffer, static_cast<DWORD>(std::size(buffer)));
  if (module_len > 0 && module_len < std::size(buffer)) {
    g_hook_dll_path.assign(buffer, module_len);
  }
}

/// Resolves the ntdll file-open stubs we detour by name. They are plain
/// exports of ntdll (already mapped into every process), so a
/// GetModuleHandle/GetProcAddress pair avoids taking a link-time dependency
/// on an ntdll import library for functions the SDK does not uniformly
/// provide one for.
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

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID /*reserved*/) {
  if (::DetourIsHelperProcess()) {
    return TRUE;
  }

  if (reason == DLL_PROCESS_ATTACH) {
    ::DetourRestoreAfterWith();
    ::DisableThreadLibraryCalls(hinst);
    read_env_config(hinst);
    resolve_ntdll_targets();

    ::DetourTransactionBegin();
    ::DetourUpdateThread(::GetCurrentThread());
    // Guard each Nt attach: a null target (GetProcAddress failed) would
    // otherwise fail the whole transaction and leave nothing hooked.
    if (TrueNtCreateFile != nullptr) {
      ::DetourAttach(&(PVOID&)TrueNtCreateFile, HookedNtCreateFile);
    }
    if (TrueNtOpenFile != nullptr) {
      ::DetourAttach(&(PVOID&)TrueNtOpenFile, HookedNtOpenFile);
    }
    ::DetourAttach(&(PVOID&)TrueCreateProcessW, HookedCreateProcessW);
    ::DetourAttach(&(PVOID&)TrueCreateProcessA, HookedCreateProcessA);
    ::DetourTransactionCommit();
  } else if (reason == DLL_PROCESS_DETACH) {
    ::DetourTransactionBegin();
    ::DetourUpdateThread(::GetCurrentThread());
    if (TrueNtCreateFile != nullptr) {
      ::DetourDetach(&(PVOID&)TrueNtCreateFile, HookedNtCreateFile);
    }
    if (TrueNtOpenFile != nullptr) {
      ::DetourDetach(&(PVOID&)TrueNtOpenFile, HookedNtOpenFile);
    }
    ::DetourDetach(&(PVOID&)TrueCreateProcessW, HookedCreateProcessW);
    ::DetourDetach(&(PVOID&)TrueCreateProcessA, HookedCreateProcessA);
    ::DetourTransactionCommit();
  }
  return TRUE;
}
