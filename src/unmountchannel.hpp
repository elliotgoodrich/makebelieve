// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace makebelieve {

/// @class UnmountChannel
/// Owns a per-mount control endpoint and provides a `std::stop_token` that is
/// signalled when another process asks the mount to shut down. The endpoint is
/// named from a hash of the mountpoint's canonical path.
class UnmountChannel {
  class Impl;
  std::unique_ptr<Impl> m_impl;

  // Derives a stable 16-hex-character id from @a key, the canonical form of a
  // mountpoint. Deterministic across processes.
  [[nodiscard]] static std::string endpoint_id(
      const std::filesystem::path& key);

  // Blocks until @a mountpoint no longer exists, up to a fixed timeout. Returns
  // true if it went away, or false if it was still present when the wait
  // elapsed.
  [[nodiscard]] static bool wait_until_gone(
      const std::filesystem::path& mountpoint);

 public:
  /// The outcome of `request_unmount`.
  enum class Result {
    ok,                ///< The mount was found, signalled, and has torn down.
    not_mounted,       ///< Nothing was serving the mountpoint.
    teardown_timeout,  ///< Signalled, but still present when the wait elapsed.
  };

  /// Signals the instance serving @a mountpoint to shut down and blocks until
  /// it has torn down, up to a fixed timeout. Returns `Result::ok` on a clean
  /// teardown, `Result::not_mounted` if nothing was serving @a mountpoint, or
  /// `Result::teardown_timeout` if it was signalled but had not gone within the
  /// timeout.
  [[nodiscard]] static Result request_unmount(
      const std::filesystem::path& mountpoint);

  /// Creates the control endpoint for @a mountpoint and starts a listener that
  /// signals `token()` when the endpoint is poked. If no per-user runtime
  /// directory is available (Linux), it warns and runs without an endpoint:
  /// `token()` never fires and `request_unmount` cannot reach this mount.
  /// @throws std::system_error if the endpoint cannot be created.
  /// @throws std::runtime_error if another live instance already owns it.
  explicit UnmountChannel(const std::filesystem::path& mountpoint);

  /// Stops the listener and removes the endpoint.
  ~UnmountChannel();

  UnmountChannel(const UnmountChannel&) = delete;
  UnmountChannel& operator=(const UnmountChannel&) = delete;
  UnmountChannel(UnmountChannel&&) = delete;
  UnmountChannel& operator=(UnmountChannel&&) = delete;

  /// Provides a `std::stop_token` that is signalled when the endpoint is poked.
  /// `std::stop_token`s returned from this function will not be triggered after
  /// the creating `UnmountChannel` is destroyed.
  [[nodiscard]] std::stop_token token() const;
};

}  // namespace makebelieve
