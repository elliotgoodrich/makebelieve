// SPDX-License-Identifier: MIT
#pragma once

#include "nativehandle.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace makebelieve {

/// @class NativeHandlePoller
/// This is a mechanism to wait on multiple operating-system handles at once,
/// while being able to be woken up from another thread.
class NativeHandlePoller {
  struct Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// Creates a `NativeHandlePoller` currently waiting on no handles.
  /// @throws std::system_error if the platform's wait cannot be set up.
  NativeHandlePoller();

  /// @pre Every `Token` from `add` has been passed to `remove`.
  ~NativeHandlePoller();

  NativeHandlePoller(const NativeHandlePoller&) = delete;
  NativeHandlePoller& operator=(const NativeHandlePoller&) = delete;
  NativeHandlePoller(NativeHandlePoller&&) = delete;
  NativeHandlePoller& operator=(NativeHandlePoller&&) = delete;

  /// Identifies one `add`, until it is passed to `remove`.
  enum class Token : std::uintptr_t {};

  /// Starts watching @a handle, with an associated @a context, until the
  /// registration returned is passed to `remove`.
  /// @throws std::system_error if @a handle cannot be watched.
  [[nodiscard]] Token add(NativeHandle handle, void* context);

  /// Stops the watch @a token identifies, after which it is not
  /// reported again.
  /// @pre @a token came from `add` and has not been removed.
  void remove(Token token) noexcept;

  /// Blocks until a watched handle is ready or `wake` is called, appending
  /// the context of each ready handle to @a ready. A handle that hangs up or
  /// fails counts as ready, leaving its owner's I/O on it to report why. May
  /// return having appended nothing.
  void wait(std::vector<void*>& ready);

  /// Makes a `wait` in progress, or the next one, return. Thread-safe.
  void wake() noexcept;
};

}  // namespace makebelieve
