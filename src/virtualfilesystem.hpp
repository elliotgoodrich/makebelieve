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
/// File opens and reads are coroutine operations with deferred native replies:
/// low-level FUSE requests on Linux and WinFsp transactions on Windows.
/// Waiting for a build occupies no dispatcher thread and has no fixed
/// admission limit. Metadata and directory operations remain synchronous.
/// File continuations use a dedicated executor, separate from notification
/// work that may itself call through the mount.
/// The file coroutines are `exec::task`s started on that executor. Their
/// scheduler affinity transfers awaited sender completions there, including
/// inline stop-callback completions, before replying or destroying requests.
///
/// Linux interrupts cancel only the affected request and report EINTR;
/// shutdown cancels pending requests with ECANCELED. Windows has no per-request
/// cancellation callback in this WinFsp interface: a pending operation can
/// outlive its caller until the tree completes it or the mount shuts down.
/// Pending Windows opens also defer change notifications for their path until
/// the open fails or its resulting handle closes.
class VirtualFileSystem {
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
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
