// SPDX-License-Identifier: MIT
#include "osbuildprocesstracker.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

// A record holds the command's pid and start time, read while it is parked
// before exec as our unreaped child. A requester - a thread, as FUSE reports
// it - is mapped to its process and followed up its parents to a command in
// the tracker, matched on pid and start time both, so a recycled pid is never
// found to belong to a build that has ended. The process ProcessUtil adds is
// the init of the command's own pid namespace, so a process orphaned within
// it - a daemon, say - is re-parented to that init, and so is still found.
// Ids are the ones this process sees, in its own pid namespace, as are those
// FUSE reports for requests to a mount made there.
//
// /proc is read outside the lock; only comparing against the records holds
// it.

namespace makebelieve {

namespace {

// What /proc says of a process: its parent, and when it started (in clock
// ticks since boot), which with its pid names it uniquely.
struct Stat {
  std::uint32_t parent;
  std::uint64_t start_time;
};

// Reads /proc/<pid>/stat, or nothing if the process has gone.
std::optional<Stat> stat_of(std::uint32_t pid) {
  std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
  const std::string line((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  // The command name, in parentheses, may itself hold spaces and
  // parentheses: the fields proper start after the last ')'.
  const std::size_t close = line.rfind(')');
  if (close == std::string::npos) {
    return std::nullopt;
  }
  std::istringstream fields(line.substr(close + 1));
  std::string state;
  std::uint32_t parent = 0;
  fields >> state >> parent;
  // Skip to field 22, starttime: fields 5 to 21 lie between.
  std::string skipped;
  for (int field = 5; field <= 21; ++field) {
    fields >> skipped;
  }
  std::uint64_t start_time = 0;
  if (!(fields >> start_time)) {
    return std::nullopt;
  }
  return Stat{.parent = parent, .start_time = start_time};
}

// The process the thread @a thread belongs to, or nothing if it has gone.
std::optional<std::uint32_t> process_of(std::uint32_t thread) {
  std::ifstream status("/proc/" + std::to_string(thread) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    constexpr std::string_view k_field = "Tgid:";
    if (line.starts_with(k_field)) {
      return static_cast<std::uint32_t>(
          std::strtoul(line.c_str() + k_field.size(), nullptr, 10));
    }
  }
  return std::nullopt;
}

struct Record {
  std::filesystem::path output;
  std::uint32_t pid;
  std::uint64_t start_time;
};

// How many parents a lookup climbs before giving up, which only a process
// tree far deeper than any build's would reach.
constexpr int k_max_depth = 4096;

}  // namespace

class OsBuildProcessTracker::Impl {
 public:
  std::mutex mutex;
  BuildProcessTracker::Token last_token = 0;
  std::unordered_map<BuildProcessTracker::Token, Record> records;
};

OsBuildProcessTracker::OsBuildProcessTracker()
    : m_impl(std::make_unique<Impl>()) {}

OsBuildProcessTracker::~OsBuildProcessTracker() = default;

std::expected<BuildProcessTracker::Token, std::error_code>
OsBuildProcessTracker::add(const std::filesystem::path& output,
                           const ProcessHandle& process) {
  // The process is parked before exec as our unreaped child, so its pid is
  // still its own while this reads it; the pidfd confirms it has not gone.
  const std::optional<Stat> stat = stat_of(process.pid);
  if (!stat.has_value()) {
    return std::unexpected(std::make_error_code(std::errc::no_such_process));
  }
  if (process.pidfd >= 0 &&
      ::syscall(SYS_pidfd_send_signal, process.pidfd, 0, nullptr, 0) != 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  Record record{
      .output = output, .pid = process.pid, .start_time = stat->start_time};
  const std::lock_guard lock(m_impl->mutex);
  const Token token = ++m_impl->last_token;
  m_impl->records.emplace(token, std::move(record));
  return token;
}

void OsBuildProcessTracker::remove(Token token) noexcept {
  const std::lock_guard lock(m_impl->mutex);
  m_impl->records.erase(token);
}

std::optional<std::filesystem::path> OsBuildProcessTracker::find(
    std::uint32_t requester_pid) {
  {
    const std::lock_guard lock(m_impl->mutex);
    if (m_impl->records.empty()) {
      return std::nullopt;
    }
  }
  // FUSE names the thread; its process is where the walk starts.
  std::optional<std::uint32_t> pid = process_of(requester_pid);
  const auto self = static_cast<std::uint32_t>(::getpid());
  for (int depth = 0;
       pid.has_value() && *pid > 1 && *pid != self && depth < k_max_depth;
       ++depth) {
    const std::optional<Stat> stat = stat_of(*pid);
    if (!stat.has_value()) {
      return std::nullopt;  // Gone mid-walk: a miss.
    }
    {
      const std::lock_guard lock(m_impl->mutex);
      for (const auto& [token, record] : m_impl->records) {
        if (record.pid == *pid && record.start_time == stat->start_time) {
          return record.output;
        }
      }
    }
    pid = stat->parent;
  }
  return std::nullopt;
}

}  // namespace makebelieve
