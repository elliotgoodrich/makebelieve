// SPDX-License-Identifier: MIT
#include "processattribution.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

namespace makebelieve {

namespace {

// One registered command: its attempt, and our own handle to its job, closed
// once nothing - a registration, or a lookup under way - refers to it.
struct Record {
  AttemptId attempt;
  HANDLE job;

  Record(AttemptId owner, HANDLE handle) : attempt(owner), job(handle) {}
  Record(const Record&) = delete;
  Record& operator=(const Record&) = delete;
  Record(Record&&) = delete;
  Record& operator=(Record&&) = delete;
  ~Record() { CloseHandle(job); }
};

}  // namespace

class ProcessAttribution::Impl {
 public:
  std::mutex mutex;
  std::vector<std::shared_ptr<const Record>> records;
};

ProcessAttribution::ProcessAttribution() : m_impl(std::make_unique<Impl>()) {}

ProcessAttribution::~ProcessAttribution() = default;

std::expected<LaunchRegistration, std::error_code>
ProcessAttribution::register_process(AttemptId attempt,
                                     const LaunchedProcess& process) {
  // Our own handle, so the job stays queryable for as long as it is
  // registered, whatever becomes of the launcher's.
  HANDLE job = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), process.job, GetCurrentProcess(),
                       &job, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return std::unexpected(std::error_code(static_cast<int>(GetLastError()),
                                           std::system_category()));
  }
  auto record = std::make_shared<const Record>(attempt, job);
  {
    const std::lock_guard lock(m_impl->mutex);
    m_impl->records.push_back(record);
  }
  return LaunchRegistration([impl = m_impl.get(), record = std::move(record)] {
    const std::lock_guard lock(impl->mutex);
    std::erase(impl->records, record);
  });
}

std::optional<AttemptId> ProcessAttribution::resolve(std::uint32_t requester) {
  std::vector<std::shared_ptr<const Record>> records;
  {
    const std::lock_guard lock(m_impl->mutex);
    records = m_impl->records;
  }
  if (records.empty()) {
    return std::nullopt;
  }
  // The requester's request is still being served, so it cannot have exited
  // and had its id reused.
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, requester);
  if (process == nullptr) {
    return std::nullopt;
  }
  std::optional<AttemptId> found;
  for (const std::shared_ptr<const Record>& record : records) {
    BOOL in_job = FALSE;
    if (IsProcessInJob(process, record->job, &in_job) && in_job) {
      found = record->attempt;
      break;
    }
  }
  CloseHandle(process);
  return found;
}

}  // namespace makebelieve
