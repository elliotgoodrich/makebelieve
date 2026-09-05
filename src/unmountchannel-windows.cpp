// SPDX-License-Identifier: MIT
#include "unmountchannel.hpp"

#include <array>
#include <filesystem>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <windows.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {static_cast<int>(GetLastError()), std::system_category()};
}

// Resolves @a mountpoint to a stable canonical form so that a relative path, an
// absolute path and any 8.3 short-name or symlinked form all hash the same. The
// mount root exists while it is served, so it can be opened and normalized;
// otherwise it falls back to a lexical absolute path.
std::wstring canonical_key(const std::filesystem::path& mountpoint) {
  const HANDLE handle =
      CreateFileW(mountpoint.c_str(), 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  std::filesystem::path resolved;
  if (handle == INVALID_HANDLE_VALUE) {
    resolved = std::filesystem::absolute(mountpoint).lexically_normal();
  } else {
    // A null buffer asks for the length including the terminator; fill in place
    // at that size. The fill returns the chars written excluding the
    // terminator, or 0 to fall back - on an error or a path that grew between
    // the calls (len >= size), leaving the string empty.
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    std::wstring text;
    text.resize_and_overwrite(needed, [&](wchar_t* buffer, std::size_t size) {
      const DWORD len =
          size == 0 ? 0
                    : GetFinalPathNameByHandleW(
                          handle, buffer, static_cast<DWORD>(size), flags);
      return len >= size ? std::size_t{0} : std::size_t{len};
    });
    CloseHandle(handle);
    if (text.empty()) {
      resolved = std::filesystem::absolute(mountpoint).lexically_normal();
    } else {
      std::wstring_view view(text);
      constexpr std::wstring_view k_extended_prefix = L"\\\\?\\";
      if (view.starts_with(k_extended_prefix)) {
        view.remove_prefix(k_extended_prefix.size());
      }
      resolved = std::filesystem::path(view);
    }
  }

  // No case-folding: FILE_NAME_NORMALIZED already yields the on-disk casing, so
  // two spellings of one directory resolve identically, while two entries that
  // differ only by case in a case-sensitive directory stay distinct.
  return resolved.wstring();
}

// Formats the event's name in the per-session Local namespace, so a mount is
// private to the user that started it, from an endpoint id. The id is ASCII
// hex, so widening it is a byte-wise copy.
std::wstring endpoint_name(const std::string& id) {
  return std::wstring(L"Local\\makebelieve-") +
         std::wstring(id.begin(), id.end());
}

// Closes a HANDLE on scope exit, tolerating a null handle.
class ScopedHandle {
  HANDLE m_handle = nullptr;

 public:
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
  ~ScopedHandle() {
    if (m_handle != nullptr) {
      CloseHandle(m_handle);
    }
  }

  void reset(HANDLE handle) {
    if (m_handle != nullptr) {
      CloseHandle(m_handle);
    }
    m_handle = handle;
  }

  [[nodiscard]] HANDLE get() const { return m_handle; }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&&) = delete;
  ScopedHandle& operator=(ScopedHandle&&) = delete;
};

}  // namespace

class UnmountChannel::Impl {
  std::stop_source m_source;
  ScopedHandle m_poke;  // Auto-reset event, signalled to trigger a stop.
  ScopedHandle m_quit;  // Signalled to stop the listener.
  std::thread m_listener;

 public:
  explicit Impl(const std::filesystem::path& mountpoint) {
    // Create the named event first and read the "already exists" flag before
    // any other Win32 call can clear it: a live event of this name means
    // another instance already owns the mount.
    const std::wstring name =
        endpoint_name(endpoint_id(canonical_key(mountpoint)));
    m_poke.reset(CreateEventW(nullptr, FALSE, FALSE, name.c_str()));
    const bool already_exists = GetLastError() == ERROR_ALREADY_EXISTS;
    if (m_poke.get() == nullptr) {
      throw std::system_error(last_error_code(),
                              "CreateEvent failed to create the control "
                              "endpoint");
    }
    if (already_exists) {
      throw std::runtime_error(
          "another makebelieve instance is already serving [" +
          mountpoint.string() + "]");
    }

    m_quit.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (m_quit.get() == nullptr) {
      throw std::system_error(last_error_code(),
                              "CreateEvent failed to create the control "
                              "endpoint");
    }

    m_listener = std::thread([this] { listen(); });
  }

  ~Impl() {
    SetEvent(m_quit.get());
    if (m_listener.joinable()) {
      m_listener.join();
    }
    // Closing the last handle to the named event removes the endpoint.
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] std::stop_token token() const { return m_source.get_token(); }

 private:
  void listen() {
    const std::array<HANDLE, 2> handles{m_poke.get(), m_quit.get()};
    while (true) {
      const DWORD waited = WaitForMultipleObjects(
          static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
      if (waited == WAIT_OBJECT_0) {
        m_source.request_stop();  // The poke; idempotent, keep listening.
      } else {
        return;  // The quit signal, or a wait failure we cannot recover from.
      }
    }
  }
};

UnmountChannel::Result UnmountChannel::request_unmount(
    const std::filesystem::path& mountpoint) {
  const std::wstring name =
      endpoint_name(endpoint_id(canonical_key(mountpoint)));
  const HANDLE handle =
      OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name.c_str());
  if (handle == nullptr) {
    return Result::not_mounted;  // No endpoint of that name.
  }
  const ScopedHandle guard(handle);

  if (!SetEvent(handle)) {
    throw std::system_error(last_error_code(),
                            "SetEvent failed to signal the mount");
  }

  return wait_until_gone(mountpoint) ? Result::ok : Result::teardown_timeout;
}

UnmountChannel::UnmountChannel(const std::filesystem::path& mountpoint)
    : m_impl(std::make_unique<Impl>(mountpoint)) {}

UnmountChannel::~UnmountChannel() = default;

std::stop_token UnmountChannel::token() const {
  return m_impl->token();
}

}  // namespace makebelieve
