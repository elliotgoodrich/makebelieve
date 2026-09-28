// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <system_error>
#include <utility>

namespace makebelieve {

class AdmissionBudget;

/// @class AdmissionToken
/// One unit of an @link AdmissionBudget, reserved for a request that is
/// about to wait and given back when this is destroyed - on every way out of
/// the wait, cancellation and exceptions included. Move-only.
class AdmissionToken {
  friend class AdmissionBudget;

  AdmissionBudget* m_budget = nullptr;

  explicit AdmissionToken(AdmissionBudget& budget) noexcept
      : m_budget(&budget) {}

 public:
  /// Holds nothing.
  AdmissionToken() = default;

  AdmissionToken(AdmissionToken&& other) noexcept
      : m_budget(std::exchange(other.m_budget, nullptr)) {}

  AdmissionToken& operator=(AdmissionToken other) noexcept {
    std::swap(m_budget, other.m_budget);
    return *this;
  }

  AdmissionToken(const AdmissionToken&) = delete;

  ~AdmissionToken();

  /// Whether this holds a unit.
  [[nodiscard]] explicit operator bool() const noexcept {
    return m_budget != nullptr;
  }
};

/// @class AdmissionBudget
/// How many requests may be blocked waiting at once - one unit each, taken by
/// @link try_reserve before a request waits and given back when it stops. A
/// filesystem sizes one to the dispatcher threads it can let park, so that
/// waiting requests can never take every thread and leave none to serve what
/// they are waiting on.
///
/// Reserving is a single atomic step that never blocks: two requests racing
/// for the last unit cannot both have it, which checking how many are free
/// and then waiting could not promise.
class AdmissionBudget {
  friend class AdmissionToken;

  std::atomic<std::size_t> m_free;
  std::size_t m_capacity;

  void give_back() noexcept { m_free.fetch_add(1, std::memory_order_release); }

 public:
  /// A budget of @a capacity units.
  explicit AdmissionBudget(std::size_t capacity) noexcept
      : m_free(capacity), m_capacity(capacity) {}

  AdmissionBudget(const AdmissionBudget&) = delete;
  AdmissionBudget& operator=(const AdmissionBudget&) = delete;
  AdmissionBudget(AdmissionBudget&&) = delete;
  AdmissionBudget& operator=(AdmissionBudget&&) = delete;

  /// @pre Every token it handed out has been given back.
  ~AdmissionBudget() = default;

  /// Takes a unit, or fails with `std::errc::resource_unavailable_try_again`
  /// if none is free. Never blocks.
  [[nodiscard]] std::expected<AdmissionToken, std::error_code>
  try_reserve() noexcept {
    std::size_t free = m_free.load(std::memory_order_relaxed);
    do {
      if (free == 0) {
        return std::unexpected(
            std::make_error_code(std::errc::resource_unavailable_try_again));
      }
    } while (!m_free.compare_exchange_weak(
        free, free - 1, std::memory_order_acquire, std::memory_order_relaxed));
    return AdmissionToken(*this);
  }

  /// The units not reserved right now.
  [[nodiscard]] std::size_t available() const noexcept {
    return m_free.load(std::memory_order_acquire);
  }

  /// The units there are in all.
  [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
};

inline AdmissionToken::~AdmissionToken() {
  if (m_budget != nullptr) {
    m_budget->give_back();
  }
}

}  // namespace makebelieve
