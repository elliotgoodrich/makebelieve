/**
 * @file
 * Payload DLL injected (via DetourCreateProcessWithDllExW - see
 * detours_tracing.m.cpp) into the traced command and, transitively, every
 * child process it spawns. Detours CreateFileW/CreateFileA and
 * CreateProcessW/CreateProcessA:
 *
 *   - CreateFileW/A: records the resolved path of every successful,
 *     read-access, non-directory open that falls under the traced root
 *     (both passed in via environment variables the launcher sets before
 *     spawning), appending it to a shared log file.
 *   - CreateProcessW/A: re-injects this same DLL into every child process,
 *     via the same DetourCreateProcessWithDllEx mechanism the launcher
 *     itself uses, so descendant processes are traced too - something
 *     FUSE_tracing gets for free from being mount-scoped rather than
 *     process-scoped (see ../ETW_tracing/README.md for the same point
 *     made about ETW, which gets it for free from being system-scoped
 *     instead).
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
// and fails with "Unknown architecture".
#include <windows.h>

#include <detours.h>

#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>

namespace {

constexpr wchar_t kLogEnvVar[] = L"MAKEBELIEVE_DETOURS_TRACING_LOG";
constexpr wchar_t kRootEnvVar[] = L"MAKEBELIEVE_DETOURS_TRACING_ROOT";

std::wstring g_log_path;
std::wstring g_root_prefix_lower;  // lowercased, with a trailing separator
std::wstring g_hook_dll_path;

decltype(&::CreateFileW) TrueCreateFileW = ::CreateFileW;
decltype(&::CreateFileA) TrueCreateFileA = ::CreateFileA;
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

std::wstring to_wide(std::string_view narrow) {
  if (narrow.empty()) {
    return {};
  }
  const int len = ::MultiByteToWideChar(
      CP_ACP, 0, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_ACP, 0, narrow.data(),
                        static_cast<int>(narrow.size()), result.data(), len);
  return result;
}

/// Best-effort: a failure here just means one dependency goes unreported,
/// consistent with this tracer's general best-effort posture (see the
/// file comment).
///
/// Calls TrueCreateFileW, not plain ::CreateFileW: Detours patches the
/// process-wide CreateFileW entry point, so a call through the bare name
/// here would re-enter HookedCreateFileW recursively (bounded - the log
/// file's own FILE_APPEND_DATA-only access fails record_if_interesting's
/// GENERIC_READ check and returns immediately - but still pointless
/// extra work through the whole detour machinery on every single logged
/// access, and one less thing to reason about being safe to do from
/// inside a hook that can fire arbitrarily early in a process's life).
void append_log_line(std::wstring_view path) {
  if (g_log_path.empty()) {
    return;
  }
  const HANDLE h = TrueCreateFileW(g_log_path.c_str(), FILE_APPEND_DATA,
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

/// Records `raw_path` if it was opened with read access, resolves to an
/// existing non-directory file, and falls under the traced root.
/// `raw_path` is exactly what was passed to CreateFileW/A - not yet
/// canonicalized - so it's resolved against the process's own current
/// directory via GetFullPathNameW before the boundary check.
void record_if_interesting(std::wstring_view raw_path,
                           DWORD desired_access,
                           HANDLE handle) {
  if (handle == INVALID_HANDLE_VALUE || (desired_access & GENERIC_READ) == 0 ||
      g_root_prefix_lower.empty()) {
    return;
  }

  BY_HANDLE_FILE_INFORMATION info{};
  if (!::GetFileInformationByHandle(handle, &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return;
  }

  wchar_t full[32768];
  const DWORD full_len =
      ::GetFullPathNameW(std::wstring(raw_path).c_str(),
                         static_cast<DWORD>(std::size(full)), full, nullptr);
  if (full_len == 0 || full_len >= std::size(full)) {
    return;
  }

  const std::wstring_view resolved(full, full_len);
  const std::wstring lower = to_lower(resolved);
  if (lower.size() <= g_root_prefix_lower.size() ||
      lower.compare(0, g_root_prefix_lower.size(), g_root_prefix_lower) != 0) {
    return;  // outside the current directory - not tracked, same as ".."
  }

  append_log_line(resolved);
}

HANDLE WINAPI HookedCreateFileW(LPCWSTR file_name,
                                DWORD desired_access,
                                DWORD share_mode,
                                LPSECURITY_ATTRIBUTES security_attributes,
                                DWORD creation_disposition,
                                DWORD flags_and_attributes,
                                HANDLE template_file) {
  const HANDLE h = TrueCreateFileW(file_name, desired_access, share_mode,
                                   security_attributes, creation_disposition,
                                   flags_and_attributes, template_file);
  // Guard against constructing a wstring_view from a null file_name -
  // that would call wcslen(nullptr) while binding the argument below,
  // before record_if_interesting's own body ever runs.
  if (file_name != nullptr) {
    record_if_interesting(file_name, desired_access, h);
  }
  return h;
}

HANDLE WINAPI HookedCreateFileA(LPCSTR file_name,
                                DWORD desired_access,
                                DWORD share_mode,
                                LPSECURITY_ATTRIBUTES security_attributes,
                                DWORD creation_disposition,
                                DWORD flags_and_attributes,
                                HANDLE template_file) {
  const HANDLE h = TrueCreateFileA(file_name, desired_access, share_mode,
                                   security_attributes, creation_disposition,
                                   flags_and_attributes, template_file);
  // Same null guard as HookedCreateFileW above, for the string_view
  // to_wide() constructs from file_name.
  if (file_name != nullptr) {
    record_if_interesting(to_wide(file_name), desired_access, h);
  }
  return h;
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

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID /*reserved*/) {
  if (::DetourIsHelperProcess()) {
    return TRUE;
  }

  if (reason == DLL_PROCESS_ATTACH) {
    ::DetourRestoreAfterWith();
    ::DisableThreadLibraryCalls(hinst);
    read_env_config(hinst);

    ::DetourTransactionBegin();
    ::DetourUpdateThread(::GetCurrentThread());
    ::DetourAttach(&(PVOID&)TrueCreateFileW, HookedCreateFileW);
    ::DetourAttach(&(PVOID&)TrueCreateFileA, HookedCreateFileA);
    ::DetourAttach(&(PVOID&)TrueCreateProcessW, HookedCreateProcessW);
    ::DetourAttach(&(PVOID&)TrueCreateProcessA, HookedCreateProcessA);
    ::DetourTransactionCommit();
  } else if (reason == DLL_PROCESS_DETACH) {
    ::DetourTransactionBegin();
    ::DetourUpdateThread(::GetCurrentThread());
    ::DetourDetach(&(PVOID&)TrueCreateFileW, HookedCreateFileW);
    ::DetourDetach(&(PVOID&)TrueCreateFileA, HookedCreateFileA);
    ::DetourDetach(&(PVOID&)TrueCreateProcessW, HookedCreateProcessW);
    ::DetourDetach(&(PVOID&)TrueCreateProcessA, HookedCreateProcessA);
    ::DetourTransactionCommit();
  }
  return TRUE;
}
