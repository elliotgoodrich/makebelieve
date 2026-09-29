// SPDX-License-Identifier: MIT
#include "processattribution.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

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
  AttemptId attempt;
  std::uint32_t pid;
  std::uint64_t start_time;
};

// How many parents a lookup climbs before giving up, which only a process
// tree far deeper than any build's would reach.
constexpr int k_max_depth = 4096;

}  // namespace

class ProcessAttribution::Impl {
 public:
  std::mutex mutex;
  std::vector<Record> records;
};

ProcessAttribution::ProcessAttribution() : m_impl(std::make_unique<Impl>()) {}

ProcessAttribution::~ProcessAttribution() = default;

std::expected<LaunchRegistration, std::error_code>
ProcessAttribution::register_process(AttemptId attempt,
                                     const LaunchedProcess& process) {
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
  const Record record{
      .attempt = attempt, .pid = process.pid, .start_time = stat->start_time};
  {
    const std::lock_guard lock(m_impl->mutex);
    m_impl->records.push_back(record);
  }
  return LaunchRegistration([impl = m_impl.get(), record] {
    const std::lock_guard lock(impl->mutex);
    std::erase_if(impl->records, [&](const Record& it) {
      return it.attempt == record.attempt && it.pid == record.pid &&
             it.start_time == record.start_time;
    });
  });
}

std::optional<AttemptId> ProcessAttribution::resolve(std::uint32_t requester) {
  {
    const std::lock_guard lock(m_impl->mutex);
    if (m_impl->records.empty()) {
      return std::nullopt;
    }
  }
  // FUSE names the thread; its process is where the walk starts.
  std::optional<std::uint32_t> pid = process_of(requester);
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
      for (const Record& record : m_impl->records) {
        if (record.pid == *pid && record.start_time == stat->start_time) {
          return record.attempt;
        }
      }
    }
    pid = stat->parent;
  }
  return std::nullopt;
}

}  // namespace makebelieve
