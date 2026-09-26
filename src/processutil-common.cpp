// SPDX-License-Identifier: MIT
#include "processutil.hpp"

#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

namespace makebelieve {

namespace {

// The failure due next, set by ProcessUtilTestUtil. Test-only.
std::mutex g_forced_mutex;
std::optional<std::pair<int, std::error_code>> g_forced;

}  // namespace

std::optional<std::error_code> ProcessUtilTestUtil::take(int failure) {
  const std::lock_guard lock(g_forced_mutex);
  if (!g_forced.has_value() || g_forced->first != failure) {
    return std::nullopt;
  }
  const std::error_code error = g_forced->second;
  g_forced.reset();
  return error;
}

void ProcessUtilTestUtil::fail_next(Failure failure, std::error_code error) {
  const std::lock_guard lock(g_forced_mutex);
  g_forced.emplace(static_cast<int>(failure), error);
}

void ProcessUtilTestUtil::reset() {
  const std::lock_guard lock(g_forced_mutex);
  g_forced.reset();
}

}  // namespace makebelieve
