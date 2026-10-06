// SPDX-License-Identifier: MIT
#pragma once

#include "nativehandle.hpp"

#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace makebelieve {

/// @class NativeHandlePoller
/// This is a mechanism to wait on multiple operating-system handles at once,
/// while being able to be woken up from another thread.
class NativeHandlePoller {
  struct Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// What one blocking wait turned up: the key a ready handle was registered
  /// under, and an error if waiting on it failed rather than it becoming
  /// ready.
  using Ready = std::pair<void*, std::error_code>;

  /// Creates a `NativeHandlePoller` currently waiting on no handles.
  /// @throws std::system_error if the platform's wait cannot be set up.
  NativeHandlePoller();
  ~NativeHandlePoller();

  NativeHandlePoller(const NativeHandlePoller&) = delete;
  NativeHandlePoller& operator=(const NativeHandlePoller&) = delete;
  NativeHandlePoller(NativeHandlePoller&&) = delete;
  NativeHandlePoller& operator=(NativeHandlePoller&&) = delete;

  /// Starts watching @a handle, reporting it under @a key; returns an error if
  /// it cannot be watched.
  [[nodiscard]] std::error_code add(NativeHandle handle, void* key);

  /// Stops watching @a handle, which was previously added under @a key.
  void remove(NativeHandle handle, void* key) noexcept;

  /// Blocks until a watched handle is ready or `wake` is called, appending
  /// what is ready to @a ready. May return having appended nothing.
  void wait(std::vector<Ready>& ready);

  /// Makes a `wait` in progress, or the next one, return. Thread-safe.
  void wake() noexcept;
};

}  // namespace makebelieve
