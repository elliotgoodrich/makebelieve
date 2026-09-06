// SPDX-License-Identifier: MIT
/**
 * @file
 * Arms a ReadDirectoryChangesW watch on a directory, runs a trigger command,
 * then waits for a change notification naming a given file. The Windows
 * counterpart of notifyprobe-linux.m.cpp's inotify probe.
 *
 * This reports only that the change was *announced*, not that a following read
 * sees the new content. On ProjFS a directory notification is not an
 * update-completion barrier: it can be delivered while the placeholder update
 * is still in flight, so a read racing it may still return the old version. The
 * caller validates content separately, by re-reading until it settles.
 *
 * A broad filter is used because a size/last-write-only watch is not reliably
 * signalled for these updates - the placeholder swap surfaces as a file-name
 * add/remove. FILE_NOTIFY_CHANGE_LAST_ACCESS is deliberately left out so a read
 * trigger's own access is not mistaken for a change, mirroring the inotify
 * probe's IN_MODIFY.
 *
 * usage: notifyprobe <dir> <name> <timeout_ms> -- <trigger> [args...]
 * exit:  0 notification seen, 1 timed out, 2 usage or setup error.
 */

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

// ReadDirectoryChangesW fills this with FILE_NOTIFY_INFORMATION records. Must
// be DWORD-aligned and large enough that one poll's changes rarely overflow.
constexpr DWORD k_buffer_size = 4096;

// A broad change filter: the placeholder swap shows up as a file-name
// add/remove and hydration refreshes size and last-write, so watch all of
// those. Last-access is excluded so a read trigger's own access does not count.
constexpr DWORD k_filter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_CREATION;

// Appends one argument to a CreateProcess command line, following the parsing
// rules CommandLineToArgvW documents: wrap the argument in quotes and
// backslash-escape any embedded quote along with the run of backslashes ahead
// of it.
void append_arg(std::string& command_line, const char* arg) {
  if (!command_line.empty()) {
    command_line.push_back(' ');
  }
  command_line.push_back('"');
  std::size_t backslashes = 0;
  for (const char* it = arg;; ++it) {
    if (*it == '\\') {
      ++backslashes;
      continue;
    }
    if (*it == '\0') {
      command_line.append(2 * backslashes, '\\');
      break;
    }
    // A quote needs the preceding backslashes doubled and itself escaped;
    // anything else leaves them as they are.
    command_line.append(*it == '"' ? 2 * backslashes + 1 : backslashes, '\\');
    command_line.push_back(*it);
    backslashes = 0;
  }
  command_line.push_back('"');
}

// Runs argv[0..] to completion; returns false if it could not be launched.
bool run_trigger(char** argv) {
  std::string command_line;
  for (char** arg = argv; *arg != nullptr; ++arg) {
    append_arg(command_line, *arg);
  }

  STARTUPINFOA startup{.cb = sizeof(STARTUPINFOA)};
  PROCESS_INFORMATION process{};
  // lpCommandLine has to be writable, which is why command_line owns its bytes.
  if (::CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &startup, &process) == 0) {
    return false;
  }
  ::WaitForSingleObject(process.hProcess, INFINITE);
  ::CloseHandle(process.hProcess);
  ::CloseHandle(process.hThread);
  return true;
}

// Widens a narrow (UTF-8) string for comparison against the wide names
// ReadDirectoryChangesW reports.
std::wstring widen(std::string_view narrow) {
  if (narrow.empty()) {
    return {};
  }
  const int length = ::MultiByteToWideChar(
      CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, narrow.data(),
                        static_cast<int>(narrow.size()), wide.data(), length);
  return wide;
}

// Whether one filled notification buffer names `target`.
bool names_target(const BYTE* buffer, DWORD bytes, std::wstring_view target) {
  for (std::size_t offset = 0; offset < bytes;) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* info =
        reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    // info->FileName is a counted, un-terminated run of wchar_t.
    const std::wstring_view name(info->FileName,
                                 info->FileNameLength / sizeof(wchar_t));
    if (name == target) {
      return true;
    }
    if (info->NextEntryOffset == 0) {
      break;
    }
    offset += info->NextEntryOffset;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6 || std::string_view(argv[4]) != "--") {
    std::fprintf(stderr,
                 "usage: %s <dir> <name> <timeout_ms> -- <trigger> [args...]\n",
                 argv[0]);
    return 2;
  }
  const char* directory = argv[1];
  const std::wstring target = widen(argv[2]);
  const int timeout_ms = std::atoi(argv[3]);
  char** trigger = argv + 5;

  const HANDLE dir_handle =
      ::CreateFileA(directory, FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (dir_handle == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "notifyprobe: CreateFile(%s): error %lu\n", directory,
                 ::GetLastError());
    return 2;
  }

  const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    std::fprintf(stderr, "notifyprobe: CreateEvent: error %lu\n",
                 ::GetLastError());
    ::CloseHandle(dir_handle);
    return 2;
  }

  // Armed before the trigger runs so a change the trigger causes cannot slip
  // through before the watch is listening, mirroring how the inotify probe
  // adds its watch first.
  alignas(DWORD) BYTE buffer[k_buffer_size];
  OVERLAPPED overlapped = {.hEvent = event};
  DWORD unused = 0;
  if (::ReadDirectoryChangesW(dir_handle, buffer, k_buffer_size, FALSE,
                              k_filter, &unused, &overlapped, nullptr) == 0) {
    std::fprintf(stderr, "notifyprobe: ReadDirectoryChangesW: error %lu\n",
                 ::GetLastError());
    ::CloseHandle(event);
    ::CloseHandle(dir_handle);
    return 2;
  }

  if (!run_trigger(trigger)) {
    std::fprintf(stderr, "notifyprobe: CreateProcess: error %lu\n",
                 ::GetLastError());
    ::CancelIoEx(dir_handle, &overlapped);
    ::CloseHandle(event);
    ::CloseHandle(dir_handle);
    return 2;
  }

  // Wait for a change notification naming the file. Any of them counts - a read
  // trigger's hydration touch included - since all this proves is that the
  // filesystem announced the change; the caller confirms the new content.
  bool notified = false;
  bool armed = true;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())
            .count();
    if (remaining <= 0) {
      break;
    }

    if (::WaitForSingleObject(event, static_cast<DWORD>(remaining)) !=
        WAIT_OBJECT_0) {
      break;  // Deadline reached (or an error): report a timeout.
    }

    DWORD bytes = 0;
    if (::GetOverlappedResult(dir_handle, &overlapped, &bytes, FALSE) == 0) {
      break;
    }
    // The overlapped read has completed, so nothing is pending to cancel until
    // the watch is re-armed below. bytes == 0 means the buffer overflowed.
    armed = false;
    if (names_target(buffer, bytes, target)) {
      notified = true;
      break;
    }

    ::ResetEvent(event);
    if (::ReadDirectoryChangesW(dir_handle, buffer, k_buffer_size, FALSE,
                                k_filter, &unused, &overlapped, nullptr) == 0) {
      break;
    }
    armed = true;
  }

  if (armed) {
    // Drain the still-pending read so its OVERLAPPED isn't left dangling.
    ::CancelIoEx(dir_handle, &overlapped);
    DWORD discard = 0;
    ::GetOverlappedResult(dir_handle, &overlapped, &discard, TRUE);
  }
  ::CloseHandle(event);
  ::CloseHandle(dir_handle);
  return notified ? 0 : 1;
}
