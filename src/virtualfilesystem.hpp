// SPDX-License-Identifier: MIT
#pragma once

#include <exec/static_thread_pool.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>

namespace makebelieve {

class DirectoryTree;

/// @class VirtualFileSystem
/// Exposes a @link DirectoryTree as a real directory on the filesystem.
///
/// Requests are served synchronously by a pool of dispatcher threads - WinFsp's
/// on Windows, libfuse's multithreaded loop's on Linux - of exactly
/// @link k_dispatcher_threads, which the constructor sets and checks, failing
/// if it cannot. An `open` that the tree blocks (to build an output, say)
/// holds its thread for as long as it waits, so each open is handed, through
/// its `OpenContext`, an admission budget of @link k_blocking_opens units: a
/// request that would block must reserve one first, and fails with
/// `std::errc::resource_unavailable_try_again` (`EAGAIN`; on Windows
/// `STATUS_INSUFFICIENT_RESOURCES`) rather than park when none is left.
///
/// That keeps @link k_dispatcher_headroom threads from ever being parked by a
/// blocking open. They are not kept idle - any other request may occupy them -
/// but every request other than a blocking open finishes without waiting on a
/// build: stats, reads, listings, opens of what is already up to date, closes,
/// and the Linux notifier's own writes, each of which takes only short-lived
/// locks. So those requests, which are what a build's commands need to make
/// progress, are always served, however many opens are parked. What still can
/// hold a thread longer: an open waiting on a build (bounded by the budget,
/// and by the build), and on Windows a parked open outliving WinFsp's IRP
/// timeout, which fails the caller's request while the thread stays parked
/// until the build it waits on ends.
class VirtualFileSystem {
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// How many requests may be parked in a blocking open at once.
  static constexpr std::size_t k_blocking_opens = 64;

  /// How many dispatcher threads a blocking open can never occupy.
  static constexpr std::size_t k_dispatcher_headroom = 8;

  /// How many dispatcher threads serve the mount.
  static constexpr std::size_t k_dispatcher_threads =
      k_blocking_opens + k_dispatcher_headroom;

  /// Mounts @a tree at @a mountpoint, so that its contents appear as a real
  /// directory on the filesystem. The mount stays in sync with @a tree,
  /// reflecting any changes made to it for as long as this object lives; the
  /// change notifications that tell anything watching the mount are sent from
  /// @a scheduler, one pass at a time.
  ///
  /// \pre @a tree, and the pool behind @a scheduler, must outlive this object.
  /// \throws std::system_error if the mount cannot be established.
  /// \throws std::filesystem::filesystem_error if @a mountpoint already exists
  /// or cannot be created.
  VirtualFileSystem(const DirectoryTree& tree,
                    const std::filesystem::path& mountpoint,
                    exec::static_thread_pool::scheduler scheduler);

  /// Unmounts the filesystem and attempts to remove the mountpoint directory
  /// that the constructor created. This blocks until the mount has fully
  /// stopped.
  ~VirtualFileSystem();

  VirtualFileSystem(const VirtualFileSystem&) = delete;
  VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;
  VirtualFileSystem(VirtualFileSystem&&) = delete;
  VirtualFileSystem& operator=(VirtualFileSystem&&) = delete;
};

}  // namespace makebelieve
