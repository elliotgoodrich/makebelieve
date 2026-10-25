// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace makebelieve {

class IoContext;

/// @class UnmountChannel
/// Owns a per-mount control endpoint and provides a `std::stop_token` that is
/// signalled when another process asks the mount to shut down. The endpoint is
/// named from a hash of the mountpoint's canonical path. Whoever asks is held
/// until the `UnmountChannel` is destroyed, so a daemon that destroys it last
/// lets `request_unmount` return only once everything else has torn down.
///
/// The channel is created before it listens, so it can outlive what is built
/// after it, but its endpoint only once the mount is up - `listen` - as the
/// endpoint is named from the mount root.
class UnmountChannel {
  class Impl;
  std::stop_source m_source;
  std::unique_ptr<Impl> m_impl;  // Null until `listen`.

  // Derives a stable 16-hex-character id from @a key, the canonical form of a
  // mountpoint. Deterministic across processes.
  [[nodiscard]] static std::string endpoint_id(
      const std::filesystem::path& key);

 public:
  /// The outcome of `request_unmount`.
  enum class Result {
    ok,           ///< The mount was found and has torn down.
    not_mounted,  ///< Nothing was serving the mountpoint.
  };

  /// Signals the instance serving @a mountpoint to shut down and blocks until
  /// it has torn down - its `UnmountChannel` is destroyed, or its process has
  /// exited - however long that takes. Returns `Result::ok` once it has, or
  /// `Result::not_mounted` if nothing was serving @a mountpoint. For an
  /// instance whose `listen` failed partway, it returns instead once the
  /// thread that called `listen` has ended (Windows), or once the endpoint
  /// has closed (Linux), which may be before that instance has unwound.
  [[nodiscard]] static Result request_unmount(
      const std::filesystem::path& mountpoint);

  /// Creates a channel that is not yet listening.
  UnmountChannel();

  /// Stops listening, removes the endpoint and releases every
  /// `request_unmount` waiting on it.
  /// @pre Called on the thread that called `listen`, if it was called, and
  /// not the thread of the `IoContext` it listens through.
  ~UnmountChannel();

  UnmountChannel(const UnmountChannel&) = delete;
  UnmountChannel& operator=(const UnmountChannel&) = delete;
  UnmountChannel(UnmountChannel&&) = delete;
  UnmountChannel& operator=(UnmountChannel&&) = delete;

  /// Creates the control endpoint for @a mountpoint and listens on it through
  /// @a io, signalling `token()` when the endpoint is poked. If no private
  /// per-user runtime directory is usable (Linux), it warns and runs without
  /// an endpoint: `token()` never fires and `request_unmount` cannot reach
  /// this mount.
  /// @throws std::system_error if the endpoint cannot be created.
  /// @throws std::runtime_error if the endpoint is taken: by another live
  /// instance, or by one still tearing down - on Windows, that includes the
  /// moment after its `request_unmount` returns, and while any other
  /// `request_unmount` for it has yet to. On Linux, finding a live instance
  /// pokes it, as `request_unmount` does. Also (Linux) if the endpoint's path
  /// is too long.
  /// @pre `listen` has not been called on this object yet.
  /// @pre @a io outlives this object.
  void listen(const std::filesystem::path& mountpoint, IoContext& io);

  /// Provides a `std::stop_token` that is signalled when the endpoint `listen`
  /// creates is poked. One taken before `listen` is signalled all the same.
  /// `std::stop_token`s returned from this function will not be triggered
  /// after this `UnmountChannel` is destroyed.
  [[nodiscard]] std::stop_token token() const;
};

}  // namespace makebelieve
