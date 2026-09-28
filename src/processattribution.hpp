// SPDX-License-Identifier: MIT
#pragma once

#include "attribution.hpp"
#include "launchregistrar.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <system_error>

namespace makebelieve {

/// @class ProcessAttribution
/// The platform's @link Attribution: tells which build attempt, if any, the
/// process that made a filesystem request belongs to. The only place that
/// knows how processes are grouped on each platform.
///
/// - Windows: a command and everything it starts share a job, which nothing
///   can break away from. A registration keeps its own handle to that job,
///   and a requester belongs to the attempt whose job it is in. The requester
///   cannot exit, and its id be reused, while its request is being served.
/// - Linux: a registration records the command's pid and start time, read
///   while it is parked before exec as our unreaped child. A requester - a
///   thread, as FUSE reports it - is mapped to its process and followed up
///   its parents to a registered command, matched on pid and start time both,
///   so a recycled pid never resolves to an attempt that has ended. A process
///   that has been re-parented away from its command (to init, say) is missed
///   and its request goes unattributed. makebelieve is not made a child
///   subreaper; a cgroup per command could close that gap later.
class ProcessAttribution {
  class Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  ProcessAttribution();

  /// @pre Every registration it handed out has been destroyed.
  ~ProcessAttribution();

  ProcessAttribution(const ProcessAttribution&) = delete;
  ProcessAttribution& operator=(const ProcessAttribution&) = delete;
  ProcessAttribution(ProcessAttribution&&) = delete;
  ProcessAttribution& operator=(ProcessAttribution&&) = delete;

  /// Records that @a process, and everything it starts, belongs to
  /// @a attempt until the registration returned is destroyed - or says why it
  /// could not, in which case the process must not be let run.
  [[nodiscard]] std::expected<LaunchRegistration, std::error_code>
  register_process(AttemptId attempt, const LaunchedProcess& process);

  /// The attempt @a requester belongs to, if any. Blocks only briefly, on the
  /// operating system: no lock is held while it asks.
  [[nodiscard]] std::optional<AttemptId> resolve(std::uint32_t requester);
};

}  // namespace makebelieve
