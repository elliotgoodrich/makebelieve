// SPDX-License-Identifier: MIT
#pragma once

#include "buildprocesstracker.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

namespace makebelieve {

/// @class OsBuildProcessTracker
/// The real @link BuildProcessTracker: which build, named by the output it
/// builds, each running process belongs to, worked out from how the operating
/// system groups processes. The only place that knows how that grouping works
/// on each platform. `makebelieve mount` makes one and hands it to the
/// BuildCoordinator.
///
/// A requester belongs to a build if it is the command that was added, or a
/// process the operating system still counts as started by that command. A
/// process the operating system has lost that link for is found to belong to
/// no build. A recycled process id is never found to belong to a build that
/// has ended.
///
/// `add`, `remove` and `find` may be called from any thread, at the same
/// time, with no synchronization between them.
class OsBuildProcessTracker final : public BuildProcessTracker {
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  /// Creates an empty tracker.
  OsBuildProcessTracker();

  /// @pre Every process it added has been removed.
  ~OsBuildProcessTracker() override;

  OsBuildProcessTracker(const OsBuildProcessTracker&) = delete;
  OsBuildProcessTracker& operator=(const OsBuildProcessTracker&) = delete;
  OsBuildProcessTracker(OsBuildProcessTracker&&) = delete;
  OsBuildProcessTracker& operator=(OsBuildProcessTracker&&) = delete;

  /// Records that @a process, and everything it starts, belongs to the build
  /// of @a output until the token returned is removed - or says why it could
  /// not, in which case the process must not be let run.
  /// @pre @a process has not yet run any of its own code.
  [[nodiscard]] std::expected<Token, std::error_code> add(
      const std::filesystem::path& output,
      const ProcessHandle& process) override;

  /// Deletes the record `add` returned @a token for.
  void remove(Token token) noexcept override;

  /// The output whose build @a requester_pid belongs to, if any.
  [[nodiscard]] std::optional<std::filesystem::path> find(
      std::uint32_t requester_pid) override;
};

}  // namespace makebelieve
