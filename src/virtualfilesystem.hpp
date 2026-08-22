// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <memory>

namespace makebelieve {

class DirectoryTree;

/// @class VirtualFileSystem
/// Exposes a @link DirectoryTree as a real directory on the filesystem.
class VirtualFileSystem {
  class Impl;
  std::unique_ptr<Impl> m_impl;

public:
  /// Mounts @a tree at @a mountpoint, so that its contents appear as a real
  /// directory on the filesystem. The mount stays in sync with @a tree,
  /// reflecting any changes made to it for as long as this object lives.
  ///
  /// \pre @a tree must outlive this object.
  /// \throws std::system_error if the mount cannot be established.
  /// \throws std::filesystem::filesystem_error if @a mountpoint already exists or cannot be created.
  VirtualFileSystem(const DirectoryTree& tree,
                    const std::filesystem::path& mountpoint);

  /// Unmounts the filesystem and attempts to remove the mountpoint directory that the
  /// constructor created. This blocks until the mount has fully stopped.
  ~VirtualFileSystem();

  VirtualFileSystem(const VirtualFileSystem&) = delete;
  VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;
  VirtualFileSystem(VirtualFileSystem&&) = delete;
  VirtualFileSystem& operator=(VirtualFileSystem&&) = delete;
};

}  // namespace makebelieve
