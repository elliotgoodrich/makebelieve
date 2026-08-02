/**
 * @file
 * Watches a file for FILE_ACTION_MODIFIED notifications over a fixed
 * window, printing one "EVENT" line per notification observed as it
 * happens (flushed immediately, not buffered until exit). ReadDirectoryChangesW
 * only watches directories, so this splits the given file path into its
 * parent directory (what's actually watched) and file name (what's
 * filtered on) itself.
 *
 * Exists so smoke_test.ps1 can verify time.txt's change-notification
 * heartbeat (see projfs.m.cpp) without depending on anything beyond the
 * Win32 API projfs_experiment itself already links against.
 *
 * Run for the duration of a whole test scenario with its stdout appended
 * to the same log file the test's own reads are recorded into (see
 * smoke_test.ps1), rather than as a one-shot "did a notification happen"
 * check - that lets the test assert not just that a notification arrived,
 * but how many arrived by which point in the scenario.
 *
 * Usage: filewatch <file-path> <duration-seconds>
 * @return 0 once the duration elapses, 2 on setup error.
 */

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

/// Size of the buffer ReadDirectoryChangesW fills with FILE_NOTIFY_INFORMATION
/// records. Must be DWORD-aligned; large enough that a burst of changes
/// within one poll iteration doesn't need more than one overflow retry.
constexpr DWORD kBufferSize = 4096;

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc != 3) {
    std::fwprintf(stderr, L"usage: %s <file-path> <duration-seconds>\n",
                  argv[0]);
    return 2;
  }
  const std::filesystem::path file_path(argv[1]);
  // A bare file name (no parent component, e.g. "time.txt") means "the
  // current directory" - parent_path() would otherwise return an empty
  // path, which CreateFileW rejects.
  const std::wstring directory =
      file_path.has_parent_path() ? file_path.parent_path().native() : L".";
  const std::wstring target_name = file_path.filename().native();
  const auto duration = std::chrono::seconds(_wtoi(argv[2]));

  const HANDLE dir_handle =
      ::CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (dir_handle == INVALID_HANDLE_VALUE) {
    std::fwprintf(stderr, L"CreateFile(%s): error %lu\n", directory.c_str(),
                  ::GetLastError());
    return 2;
  }

  const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    std::fwprintf(stderr, L"CreateEvent: error %lu\n", ::GetLastError());
    ::CloseHandle(dir_handle);
    return 2;
  }

  alignas(DWORD) BYTE buffer[kBufferSize];
  const auto deadline = std::chrono::steady_clock::now() + duration;

  bool setup_failed = false;
  while (true) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
            .count();
    if (remaining_ms <= 0) {
      break;
    }

    OVERLAPPED overlapped = {.hEvent = event};
    ::ResetEvent(event);

    // LAST_WRITE alone, not also SIZE: projfs_experiment's updates always
    // bump LastWriteTime, so that alone is enough to catch every change,
    // and watching both filters would (on this system, empirically)
    // double up into two separate notification records per single update.
    DWORD unused_bytes = 0;
    if (!::ReadDirectoryChangesW(dir_handle, buffer, kBufferSize, FALSE,
                                 FILE_NOTIFY_CHANGE_LAST_WRITE, &unused_bytes,
                                 &overlapped, nullptr)) {
      std::fwprintf(stderr, L"ReadDirectoryChangesW: error %lu\n",
                    ::GetLastError());
      setup_failed = true;
      break;
    }

    const DWORD wait_result =
        ::WaitForSingleObject(event, static_cast<DWORD>(remaining_ms));
    if (wait_result == WAIT_TIMEOUT) {
      ::CancelIoEx(dir_handle, &overlapped);
      // Drain the cancelled request so its OVERLAPPED isn't left dangling.
      DWORD discard = 0;
      ::GetOverlappedResult(dir_handle, &overlapped, &discard, TRUE);
      break;
    }

    DWORD bytes_returned = 0;
    if (!::GetOverlappedResult(dir_handle, &overlapped, &bytes_returned,
                               FALSE)) {
      break;
    }
    if (bytes_returned == 0) {
      // Notification buffer overflowed (too many changes coalesced within
      // one poll iteration); we can't know exactly how many were missed,
      // so just keep watching rather than guessing at a count.
      continue;
    }

    std::size_t offset = 0;
    while (offset < bytes_returned) {
      const auto* info =
          reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
      if (info->Action == FILE_ACTION_MODIFIED) {
        // info->FileName isn't null-terminated, so it can't be passed
        // directly to wcscmp; a wstring_view (rather than a wstring) lets
        // us bound-compare it without allocating. Case-sensitive: both
        // sides here always come from the same lowercase "time.txt".
        const std::wstring_view name(info->FileName,
                                     info->FileNameLength / sizeof(wchar_t));
        if (name.size() == target_name.size() &&
            std::wcsncmp(name.data(), target_name.c_str(), name.size()) == 0) {
          std::printf("EVENT\n");
          std::fflush(stdout);
        }
      }
      if (info->NextEntryOffset == 0) {
        break;
      }
      offset += info->NextEntryOffset;
    }
  }

  ::CloseHandle(event);
  ::CloseHandle(dir_handle);
  return setup_failed ? 2 : 0;
}
