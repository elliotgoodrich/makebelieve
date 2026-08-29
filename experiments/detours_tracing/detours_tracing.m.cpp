/**
 * @file
 * Runs a command with detours_tracing_hook.dll injected via
 * DetourCreateProcessWithDllExW, and prints every file under the current
 * directory that the command - or a descendant process it spawns, since
 * the hook re-injects itself into every child it sees - opened for
 * reading.
 *
 * Unlike FUSE_tracing, which is itself the filesystem the traced command
 * sees, this tracer works by API interposition: the injected DLL detours
 * ntdll's NtCreateFile/NtOpenFile in the traced process's own address
 * space (see detours_tracing_hook.cpp) and appends every interesting open
 * to a log file this launcher reads back once the command exits. Those two
 * stubs are the funnel every user-mode file open passes through - whether
 * the caller reached them via CreateFileW/A, the C runtime, or NtCreateFile
 * directly - so a statically-linked binary or a direct NtCreateFile caller
 * is seen just the same. What stays invisible is only an open that bypasses
 * those stubs (issuing the raw syscall itself), and a child launched other
 * than through the hooked CreateProcessW/A (e.g. NtCreateUserProcess
 * directly) - see the project's own design discussion.
 *
 * The launcher and its target must match bitness (both 32-bit or both
 * 64-bit) - DetourCreateProcessWithDllEx cannot inject across an
 * architecture boundary.
 *
 * Usage:
 * @code
 *   detours_tracing_experiment <command> [args...]
 * @endcode
 */

// windows.h must precede detours.h - see detours_tracing_hook.cpp's
// identical comment for why.
#include <windows.h>

#include <detours.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kLogEnvVar[] = L"MAKEBELIEVE_DETOURS_TRACING_LOG";
constexpr wchar_t kRootEnvVar[] = L"MAKEBELIEVE_DETOURS_TRACING_ROOT";

/// Quotes a single argument for CreateProcessW's lpCommandLine and appends
/// it to result. Same quoting logic as ../ETW_tracing/etw_tracing.m.cpp's
/// own copy, but appending into a shared buffer rather than returning a
/// new std::wstring per argument - each experiment under experiments/ is
/// self-contained (see the top-level CMakeLists.txt comment), so this
/// small helper is duplicated rather than shared.
void quote_argument(std::wstring& result, std::wstring_view arg) {
  if (!arg.empty() &&
      arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    result += arg;
    return;
  }
  result += L'"';
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
}

std::wstring build_command_line(const std::vector<wchar_t*>& argv) {
  std::wstring cmd;
  const wchar_t* sep = L"";
  for (std::size_t i = 0; i < argv.size(); ++i) {
    cmd += sep;
    quote_argument(cmd, argv[i]);
    sep = L" ";
  }
  return cmd;
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

std::wstring to_wide_utf8(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int len = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        result.data(), len);
  return result;
}

/// Locates detours_tracing_hook.dll next to this executable - both are
/// built into the same output directory (see CMakeLists.txt), the same
/// way ../ProjFS/filewatch.exe sits next to projfs_experiment.exe.
std::optional<std::filesystem::path> find_hook_dll_path() {
  wchar_t self[32768];
  const DWORD len =
      ::GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  if (len == 0 || len >= std::size(self)) {
    return std::nullopt;
  }
  std::filesystem::path path(std::wstring_view(self, len));
  path.replace_filename(L"detours_tracing_hook.dll");
  return path;
}

/// Reads back the hook DLL's UTF-8, newline-terminated log of paths -
/// read as raw bytes and decoded by hand (not via std::wifstream, whose
/// wide-character decoding depends on the active locale rather than
/// reliably meaning UTF-8). A missing log file means the command never
/// opened anything under the current directory - not an error - so this
/// returns an empty list rather than propagating the open failure.
std::vector<std::filesystem::path> read_log(
    const std::filesystem::path& log_path) {
  std::vector<std::filesystem::path> result;
  std::ifstream log(log_path, std::ios::binary);
  if (!log) {
    return result;
  }
  std::string line;
  while (std::getline(log, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    result.emplace_back(to_wide_utf8(line));
  }
  return result;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc < 2) {
    std::fwprintf(stderr, L"usage: %s <command> [args...]\n", argv[0]);
    return 2;
  }

  const std::filesystem::path cwd = std::filesystem::current_path();

  const std::optional<std::filesystem::path> dll_path = find_hook_dll_path();
  if (!dll_path.has_value() || !std::filesystem::exists(*dll_path)) {
    std::fwprintf(stderr,
                  L"detours_tracing_experiment: could not find "
                  L"detours_tracing_hook.dll next to this executable\n");
    return 1;
  }
  const std::string dll_path_utf8 = to_utf8(dll_path->wstring());

  wchar_t temp_dir[MAX_PATH];
  ::GetTempPathW(static_cast<DWORD>(std::size(temp_dir)), temp_dir);
  const std::filesystem::path log_path =
      std::filesystem::path(temp_dir) /
      (L"detours-tracing-" + std::to_wstring(::GetCurrentProcessId()) +
       L".log");
  std::filesystem::remove(log_path);  // best-effort, in case one is stale

  // Read by every (transitively injected) process via
  // read_env_config() in detours_tracing_hook.cpp.
  ::SetEnvironmentVariableW(kLogEnvVar, log_path.c_str());
  ::SetEnvironmentVariableW(kRootEnvVar, cwd.c_str());

  const std::vector<wchar_t*> command(argv + 1, argv + argc);
  std::wstring command_line = build_command_line(command);

  STARTUPINFOW si{.cb = sizeof(si)};
  PROCESS_INFORMATION pi{};
  const BOOL created = ::DetourCreateProcessWithDllExW(
      nullptr, command_line.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
      0, nullptr, cwd.c_str(), &si, &pi, dll_path_utf8.c_str(), nullptr);

  int exit_code = 1;
  if (!created) {
    const DWORD error = ::GetLastError();
    std::fwprintf(stderr,
                  L"detours_tracing_experiment: failed to start command: "
                  L"error %lu%s\n",
                  error,
                  error == ERROR_INVALID_HANDLE
                      ? L" (32/64-bit mismatch between this launcher and "
                        L"<command>?)"
                      : L"");
  } else {
    ::CloseHandle(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    exit_code = static_cast<int>(code);
  }

  std::vector<std::filesystem::path> read_files = read_log(log_path);
  std::filesystem::remove(log_path);  // best-effort cleanup

  std::ranges::sort(read_files);
  read_files.erase(std::ranges::unique(read_files).begin(), read_files.end());

  std::wprintf(L"Files read from %s:\n", cwd.c_str());
  for (const std::filesystem::path& p : read_files) {
    std::wprintf(L"  %s\n", p.c_str());
  }
  std::fflush(stdout);

  return exit_code;
}
