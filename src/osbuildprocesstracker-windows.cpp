// SPDX-License-Identifier: MIT
#include "osbuildprocesstracker.hpp"

#include "scopeguard.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <windows.h>

// A command and everything it starts share a job, which nothing can break
// away from. A record keeps its own handle to that job, and a requester
// belongs to the build whose job it is in. The requester cannot exit, and its
// id be reused, while its request is being served.

namespace makebelieve {

namespace {

// One command in the tracker: the output it builds, and our own handle to its
// job, closed when the record is removed.
struct Record {
  std::filesystem::path output;
  HANDLE job;

  Record(std::filesystem::path built, HANDLE handle)
      : output(std::move(built)), job(handle) {}
  Record(const Record&) = delete;
  Record& operator=(const Record&) = delete;
  Record(Record&&) = delete;
  Record& operator=(Record&&) = delete;
  ~Record() { CloseHandle(job); }
};

}  // namespace

class OsBuildProcessTracker::Impl {
 public:
  // Held shared by `find` while it uses a record's job, and exclusively by
  // `add` and `remove`, so a record - and its handle - is never removed while
  // a lookup is using it.
  std::shared_mutex mutex;
  BuildProcessTracker::Token last_token = 0;
  std::unordered_map<BuildProcessTracker::Token, Record> records;
};

OsBuildProcessTracker::OsBuildProcessTracker()
    : m_impl(std::make_unique<Impl>()) {}

OsBuildProcessTracker::~OsBuildProcessTracker() = default;

std::expected<BuildProcessTracker::Token, std::error_code>
OsBuildProcessTracker::add(const std::filesystem::path& output,
                           const ProcessHandle& process) {
  // Our own handle, so the job stays queryable for as long as it is in the
  // tracker, whatever becomes of the launcher's.
  HANDLE job = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), process.job, GetCurrentProcess(),
                       &job, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return std::unexpected(std::error_code(static_cast<int>(GetLastError()),
                                           std::system_category()));
  }
  const std::unique_lock lock(m_impl->mutex);
  const Token token = ++m_impl->last_token;
  m_impl->records.try_emplace(token, output, job);
  return token;
}

void OsBuildProcessTracker::remove(Token token) noexcept {
  // Waits for any lookup still using the record before closing its job.
  const std::unique_lock lock(m_impl->mutex);
  m_impl->records.erase(token);
}

std::optional<std::filesystem::path> OsBuildProcessTracker::find(
    std::uint32_t requester_pid) {
  {
    const std::shared_lock lock(m_impl->mutex);
    if (m_impl->records.empty()) {
      return std::nullopt;
    }
  }
  // Outside the lock: the requester's request is still being served, so it
  // cannot have exited and had its id reused.
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, requester_pid);
  if (process == nullptr) {
    return std::nullopt;
  }
  const ScopeGuard close_process([process] { CloseHandle(process); });
  std::optional<std::filesystem::path> found;
  {
    const std::shared_lock lock(m_impl->mutex);
    for (const auto& [token, record] : m_impl->records) {
      BOOL in_job = FALSE;
      if (IsProcessInJob(process, record.job, &in_job) && in_job) {
        found = record.output;
        break;
      }
    }
  }
  return found;
}

}  // namespace makebelieve
