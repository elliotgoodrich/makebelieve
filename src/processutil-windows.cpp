// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

#include <windows.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

// Widens a UTF-8 command into the UTF-16 CreateProcessW expects.
std::expected<std::wstring, std::error_code> to_wide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length == 0) {
    return std::unexpected(last_error_code());
  }
  // TODO: Use resize_and_overwrite
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), length);
  return wide;
}

// Closes a HANDLE on scope exit.
class ScopedHandle {
  HANDLE m_handle;

 public:
  explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}

  ~ScopedHandle() { CloseHandle(m_handle); }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&&) = delete;
  ScopedHandle& operator=(ScopedHandle&&) = delete;
};

}  // namespace

void ProcessUtil::run(const std::filesystem::path& working_directory,
                      const std::string& command,
                      const std::stop_token& stop,
                      Complete on_done) {
  // A pipe carries the child's standard output back to us; the write end is
  // inheritable so the child receives it, the read end is not.
  SECURITY_ATTRIBUTES attributes = {
      .nLength = sizeof(attributes),
      .bInheritHandle = TRUE,
  };

  HANDLE read_raw = nullptr;
  HANDLE write_raw = nullptr;
  if (!CreatePipe(&read_raw, &write_raw, &attributes, 0)) {
    on_done(std::unexpected(last_error_code()));
    return;
  }
  const ScopedHandle read_handle(read_raw);
  SetHandleInformation(read_raw, HANDLE_FLAG_INHERIT, 0);

  // CreateProcessW takes a mutable command-line buffer, so this must be a
  // writable std::wstring rather than a literal.
  std::expected<std::wstring, std::error_code> wcommand = to_wide(command);
  if (!wcommand) {
    on_done(std::unexpected(wcommand.error()));
    return;
  }

  std::wstring command_line = L"cmd.exe /c " + wcommand.value();
  STARTUPINFOW startup = {
      .cb = sizeof(startup),
      .dwFlags = STARTF_USESTDHANDLES,
      .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
      .hStdOutput = write_raw,
      .hStdError = GetStdHandle(STD_ERROR_HANDLE),
  };

  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      nullptr, working_directory.c_str(), &startup, &process);
  const std::error_code create_error =
      created ? std::error_code{} : last_error_code();
  // Close our copy of the write end so the pipe reports end of file once the
  // child exits, and so only the child can write to it.
  CloseHandle(write_raw);
  if (!created) {
    on_done(std::unexpected(create_error));
    return;
  }

  const ScopedHandle thread(process.hThread);
  const ScopedHandle handle(process.hProcess);

  // Terminate the child if a stop is requested while we wait. The handle stays
  // valid until it is closed above, so this cannot race a reused id; the
  // callback is declared last so it is destroyed - and so can no longer fire -
  // before the handle is closed. It runs synchronously from the constructor if
  // @a stop is already requested.
  // TODO: this ends cmd.exe, not a tree of grandchildren it may spawn; a job
  // object would be needed to reach those.
  const std::stop_callback on_stop(
      stop, [process] { TerminateProcess(process.hProcess, 1); });

  std::string output;
  std::array<char, 4096> buffer{};
  const auto drain = [&] {
    while (true) {
      DWORD available = 0;
      if (!PeekNamedPipe(read_raw, nullptr, 0, nullptr, &available, nullptr) ||
          available == 0) {
        return;
      }
      const DWORD want = std::min(available, static_cast<DWORD>(buffer.size()));
      DWORD read_bytes = 0;
      if (!ReadFile(read_raw, buffer.data(), want, &read_bytes, nullptr) ||
          read_bytes == 0) {
        return;
      }
      output.append(buffer.data(), read_bytes);
    }
  };

  // Drain the pipe until the direct child exits. Waiting on the process rather
  // than on pipe end-of-file is what keeps an inherited-pipe grandchild from
  // wedging us, and draining as we go keeps a full pipe from blocking writes.
  std::error_code wait_error;
  while (true) {
    drain();
    const DWORD waited = WaitForSingleObject(process.hProcess, 50);
    if (waited == WAIT_OBJECT_0) {
      drain();  // Whatever the child buffered just before it exited.
      break;
    }
    if (waited == WAIT_FAILED) {
      wait_error = last_error_code();
      break;
    }
  }

  if (wait_error) {
    on_done(std::unexpected(wait_error));
  } else if (stop.stop_requested()) {
    on_done(
        std::unexpected(std::make_error_code(std::errc::operation_canceled)));
  } else {
    on_done(std::move(output));
  }
}

}  // namespace makebelieve
