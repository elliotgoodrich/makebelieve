// SPDX-License-Identifier: MIT
#include "unmountchannel.hpp"

#include "iocontext.hpp"
#include "scopeguard.hpp"

#include <stdexec/execution.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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
    const ScopeGuard close_handle([handle] { CloseHandle(handle); });
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

// Names the mutex the mount holds until it has torn down, beside its event.
std::wstring alive_name(const std::wstring& endpoint) {
  return endpoint + L"-alive";
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
  std::stop_source m_source;  // Shares the channel's stop state.

  // Held from construction until destruction, so `request_unmount` waits on it
  // for teardown. Owned by the constructing thread, which must release it. If
  // that thread ends first it is abandoned, which ends the wait too.
  ScopedHandle m_alive;

  ScopedHandle m_poke;  // Auto-reset event, signalled to trigger a stop.

  // The wait on m_poke, stopped and joined on destruction.
  stdexec::counting_scope m_listening;

 public:
  Impl(const std::filesystem::path& mountpoint,
       IoContext& io,
       std::stop_source source)
      : m_source(std::move(source)) {
    // Take the mutex before creating the event, so whoever finds the event
    // finds the mutex held. Read each "already exists" flag before any other
    // Win32 call can clear it: either object existing means another instance
    // already owns the mount, or is still tearing down.
    const std::wstring name =
        endpoint_name(endpoint_id(canonical_key(mountpoint)));
    m_alive.reset(CreateMutexW(nullptr, TRUE, alive_name(name).c_str()));
    if (m_alive.get() == nullptr) {
      throw std::system_error(last_error_code(),
                              "CreateMutex failed to create the control "
                              "endpoint");
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      throw std::runtime_error(
          "another makebelieve instance is already serving [" +
          mountpoint.string() + "]");
    }
    // Not released on a failure below, where the destructor will not run:
    // whoever waits on it would return while the mount that failed to listen
    // is still being unwound. Left owned, it is abandoned once this thread
    // ends, after that.

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

    // The poke is idempotent, so the watch keeps going to the end.
    io.spawn_watch(
        m_poke.get(), m_listening,
        [this]() noexcept { m_source.request_stop(); },
        [](std::error_code error) noexcept {
          // Best-effort, as this runs on the IoContext's thread.
          try {
            std::println(
                stderr,
                "makebelieve: stopped watching for unmount requests: {}",
                error.message());
          } catch (...) {  // NOLINT(bugprone-empty-catch)
          }
        });
  }

  ~Impl() {
    m_listening.request_stop();
    stdexec::sync_wait(m_listening.join());
    // Closing the last handle to the named event removes the endpoint. Only
    // then is the mutex let go, so a waiting `request_unmount` returns once
    // nothing of this mount is left.
    m_poke.reset(nullptr);
    ReleaseMutex(m_alive.get());
    // Closed at once, as while it is open no later mount can take the name.
    m_alive.reset(nullptr);
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
};

UnmountChannel::Result UnmountChannel::request_unmount(
    const std::filesystem::path& mountpoint) {
  const std::wstring name =
      endpoint_name(endpoint_id(canonical_key(mountpoint)));

  // Open the mutex before poking, so this waits on the mount it pokes: while
  // the handle is open the mutex's name stays taken, so a mount started after
  // this one has gone cannot take it over. The mount holds it from before it
  // creates its event until after it closes it, so no mutex means no mount.
  const ScopedHandle alive(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                      alive_name(name).c_str()));
  if (alive.get() == nullptr) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
      return Result::not_mounted;
    }
    throw std::system_error(last_error_code(),
                            "OpenMutex failed to reach the mount");
  }

  // Poke, then wait for the mutex to be let go. With no event the mount is
  // either about to create it or has already closed it to tear down, so keep
  // trying to poke it until it turns up or the mutex is let go.
  constexpr DWORD k_retry_ms = 10;
  bool poked = false;
  while (true) {
    if (!poked) {
      const ScopedHandle poke(
          OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str()));
      if (poke.get() != nullptr) {
        if (!SetEvent(poke.get())) {
          throw std::system_error(last_error_code(),
                                  "SetEvent failed to signal the mount");
        }
        poked = true;
      } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        throw std::system_error(last_error_code(),
                                "OpenEvent failed to reach the mount");
      }
      // Closed now, so this handle cannot keep the event's name alive for a
      // mount that follows.
    }

    // WAIT_ABANDONED means the mount's process died holding it.
    const DWORD waited =
        WaitForSingleObject(alive.get(), poked ? INFINITE : k_retry_ms);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
      // Owned now, so let it go at once for the next mount to take.
      ReleaseMutex(alive.get());
      return Result::ok;
    }
    if (waited != WAIT_TIMEOUT) {
      throw std::system_error(last_error_code(),
                              "WaitForSingleObject failed to wait for the "
                              "mount to tear down");
    }
  }
}

UnmountChannel::UnmountChannel() = default;

UnmountChannel::~UnmountChannel() = default;

void UnmountChannel::listen(const std::filesystem::path& mountpoint,
                            IoContext& io) {
  assert(m_impl == nullptr && "UnmountChannel is already listening");
  m_impl = std::make_unique<Impl>(mountpoint, io, m_source);
}

}  // namespace makebelieve
