// SPDX-License-Identifier: MIT
#pragma once

#include <stdexec/execution.hpp>

#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace makebelieve {

/// A process that has been created but has not yet run any of its own code,
/// as `ProcessUtil::run` hands it to a @link LaunchRegistrar. What identifies
/// it depends on the platform; the fields another platform has no use for are
/// left at their defaults.
struct LaunchedProcess {
  /// The process id.
  std::uint32_t pid = 0;

  /// Windows: the job object the process, and everything it starts, belongs
  /// to (a `HANDLE`). Borrowed: valid only for the duration of the call.
  void* job = nullptr;

  /// Linux: a pidfd for the process. Borrowed: valid only for the duration of
  /// the call.
  int pidfd = -1;
};

/// @class LaunchRegistration
/// Whatever a @link LaunchRegistrar recorded about a launched process, undone
/// when this is destroyed. `ProcessUtil::run` holds it for as long as it
/// supervises the process: until the command has exited and whatever it
/// started has been ended (on Windows, waited for too).
class LaunchRegistration {
  std::move_only_function<void()> m_undo;

 public:
  /// Records nothing.
  LaunchRegistration() = default;

  /// Calls @a undo on destruction.
  explicit LaunchRegistration(std::move_only_function<void()> undo)
      : m_undo(std::move(undo)) {}

  LaunchRegistration(LaunchRegistration&& other) noexcept
      : m_undo(std::exchange(other.m_undo, nullptr)) {}

  LaunchRegistration& operator=(LaunchRegistration other) noexcept {
    std::swap(m_undo, other.m_undo);
    return *this;
  }

  LaunchRegistration(const LaunchRegistration&) = delete;

  ~LaunchRegistration() {
    if (m_undo) {
      m_undo();
    }
  }
};

/// @class LaunchRegistrar
/// Told about each process `ProcessUtil::run` launches after it has been
/// created and before any of its code runs - so nothing the process does can
/// come before its registration. A registrar that fails keeps the process from
/// ever running, and the run fails with its error.
///
/// A non-owning reference to the object doing the registering, which must
/// outlive every run it is given to; cheap to copy.
class LaunchRegistrar {
  using Register = std::expected<LaunchRegistration, std::error_code> (*)(
      void* target,
      const LaunchedProcess& process);

  void* m_target = nullptr;
  Register m_register = nullptr;

  LaunchRegistrar() = default;

 public:
  /// A registrar that records nothing, for runs that nothing needs to
  /// attribute. Named rather than default-constructed, so a run without
  /// registration is always a deliberate choice.
  [[nodiscard]] static LaunchRegistrar none() noexcept { return {}; }

  /// Registers through @a target, called with each `LaunchedProcess`. Never
  /// another registrar, which is copied rather than referred to.
  template <class T>
    requires(!std::same_as<std::remove_cv_t<T>, LaunchRegistrar>) &&
                requires(T& target, const LaunchedProcess& process) {
                  {
                    target(process)
                  } -> std::same_as<
                        std::expected<LaunchRegistration, std::error_code>>;
                }
  explicit LaunchRegistrar(T& target) noexcept
      : m_target(&target),
        m_register([](void* self, const LaunchedProcess& process) {
          return (*static_cast<T*>(self))(process);
        }) {}

  /// Registers @a process, or says why it could not be.
  [[nodiscard]] std::expected<LaunchRegistration, std::error_code> operator()(
      const LaunchedProcess& process) const {
    if (m_register == nullptr) {
      return LaunchRegistration();
    }
    return m_register(m_target, process);
  }
};

/// The query for the @link LaunchRegistrar a sender that launches processes
/// must register them with. It has no default: a receiver whose environment
/// does not answer it cannot run such a sender, so no wrapper along the way can
/// drop it and have processes silently go unregistered.
struct get_launch_registrar_t {
  template <class Env>
    requires requires(const Env& env, const get_launch_registrar_t& query) {
      { env.query(query) } noexcept -> std::convertible_to<LaunchRegistrar>;
    }
  [[nodiscard]] LaunchRegistrar operator()(const Env& env) const noexcept {
    return env.query(*this);
  }

  // Passed through every adaptor between the receiver and the sender that
  // launches, as the stop token is.
  [[nodiscard]] static consteval bool query(stdexec::forwarding_query_t) {
    return true;
  }
};

inline constexpr get_launch_registrar_t get_launch_registrar{};

}  // namespace makebelieve
