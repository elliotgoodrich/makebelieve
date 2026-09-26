// SPDX-License-Identifier: MIT
#pragma once

#include "nativehandle.hpp"

#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace makebelieve {

/// @class IoContextPoller
/// The platform half of `IoContext`: the handles registered with it, and one
/// blocking wait until some of them are ready or it is woken. Used only from
/// the context's thread, except for `wake`.
class IoContextPoller {
  struct State;
  std::unique_ptr<State> m_state;

 public:
  /// What one blocking wait turned up: the key a ready handle was registered
  /// under, and an error if waiting on it failed rather than it becoming
  /// ready.
  using Ready = std::pair<void*, std::error_code>;

  /// @throws std::system_error if the platform's wait cannot be set up.
  IoContextPoller();
  ~IoContextPoller();

  IoContextPoller(const IoContextPoller&) = delete;
  IoContextPoller& operator=(const IoContextPoller&) = delete;
  IoContextPoller(IoContextPoller&&) = delete;
  IoContextPoller& operator=(IoContextPoller&&) = delete;

  /// Starts watching @a handle, reporting it under @a key; the error if it
  /// cannot be watched.
  [[nodiscard]] std::error_code add(NativeHandle handle, void* key);

  /// Stops watching @a handle, which was added under @a key.
  void remove(NativeHandle handle, void* key) noexcept;

  /// Blocks until a watched handle is ready or `wake` is called, appending
  /// what is ready to @a ready. May return having appended nothing.
  void wait(std::vector<Ready>& ready);

  /// Makes a `wait` in progress, or the next one, return. Thread-safe.
  void wake() noexcept;
};

}  // namespace makebelieve
