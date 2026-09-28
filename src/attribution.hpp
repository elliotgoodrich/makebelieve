// SPDX-License-Identifier: MIT
#pragma once

#include "launchregistrar.hpp"

#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <system_error>
#include <type_traits>

// What a platform attribution adapter and the BuildCoordinator agree on: an
// opaque id for each build attempt, and the contract for telling which one a
// request came from. Depends on neither side.

namespace makebelieve {

/// Identifies one build attempt to an @link Attribution; never reused.
using AttemptId = std::uint64_t;

/// @class Attribution
/// A non-owning reference to whatever tells which build attempt, if any, a
/// request came from: the processes each attempt launches are registered with
/// it before they run, and a requester is then resolved against them. The
/// platform's is `ProcessAttribution`. Cheap to copy; the object referred to
/// must outlive every copy.
class Attribution {
  struct Operations {
    std::expected<LaunchRegistration, std::error_code> (*register_process)(
        void* self,
        AttemptId attempt,
        const LaunchedProcess& process);
    std::optional<AttemptId> (*resolve)(void* self, std::uint32_t requester);
  };

  // One table per T, built at compile time.
  template <class T>
  static const Operations* operations_for() noexcept {
    static constexpr Operations operations{
        .register_process =
            [](void* self, AttemptId attempt, const LaunchedProcess& process) {
              return static_cast<T*>(self)->register_process(attempt, process);
            },
        .resolve =
            [](void* self, std::uint32_t requester) {
              return static_cast<T*>(self)->resolve(requester);
            },
    };
    return &operations;
  }

  static constexpr Operations k_none{
      .register_process = [](void*, AttemptId, const LaunchedProcess&)
          -> std::expected<LaunchRegistration, std::error_code> {
        return LaunchRegistration();
      },
      .resolve = [](void*, std::uint32_t) -> std::optional<AttemptId> {
        return std::nullopt;
      },
  };

  void* m_self;
  const Operations* m_operations;

  Attribution(void* self, const Operations* operations) noexcept
      : m_self(self), m_operations(operations) {}

 public:
  /// Refers to @a impl, which registers through
  /// `register_process(AttemptId, const LaunchedProcess&)` and resolves
  /// through `resolve(std::uint32_t requester)`.
  template <class T>
    requires(!std::same_as<std::remove_cv_t<T>, Attribution>) &&
            requires(T& impl,
                     AttemptId attempt,
                     const LaunchedProcess& process,
                     std::uint32_t requester) {
              {
                impl.register_process(attempt, process)
              } -> std::same_as<
                    std::expected<LaunchRegistration, std::error_code>>;
              {
                impl.resolve(requester)
              } -> std::same_as<std::optional<AttemptId>>;
            }
  explicit Attribution(T& impl) noexcept
      : Attribution(&impl, operations_for<T>()) {}

  /// Attributes nothing: every request is unattributed.
  [[nodiscard]] static Attribution none() noexcept {
    return {nullptr, &k_none};
  }

  /// Records that @a process, and what it starts, belong to @a attempt, until
  /// the registration returned is destroyed.
  [[nodiscard]] std::expected<LaunchRegistration, std::error_code>
  register_process(AttemptId attempt, const LaunchedProcess& process) const {
    return m_operations->register_process(m_self, attempt, process);
  }

  /// The attempt whose registered process @a requester is, or descends from.
  [[nodiscard]] std::optional<AttemptId> resolve(
      std::uint32_t requester) const {
    return m_operations->resolve(m_self, requester);
  }
};

}  // namespace makebelieve
