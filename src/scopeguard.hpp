// SPDX-License-Identifier: MIT
#pragma once

#include <concepts>
#include <utility>

namespace makebelieve {

/// @class ScopeGuard
/// Runs `on_exit` when the guard is destroyed, in the usual reverse order of
/// declaration, unless dismiss() was called first. Lets resource cleanup sit
/// next to its acquisition instead of being repeated at every early-return
/// path.
///
/// @tparam F Callable type invoked with no arguments on destruction.
template <std::invocable F>
class ScopeGuard {
 public:
  /// @param on_exit Callable to run when the guard is destroyed.
  explicit ScopeGuard(F on_exit) : m_on_exit(std::move(on_exit)) {}

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;
  ScopeGuard(ScopeGuard&&) = delete;
  ScopeGuard& operator=(ScopeGuard&&) = delete;

  ~ScopeGuard() {
    if (m_active) {
      std::move(m_on_exit)();
    }
  }

  /// Cancels the pending call to `on_exit`; the destructor becomes a no-op.
  void dismiss() { m_active = false; }

 private:
  F m_on_exit;
  bool m_active = true;
};

}  // namespace makebelieve
