// SPDX-License-Identifier: MIT
#include "unmountchannel.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace makebelieve {

namespace {

std::error_code last_error_code() {
  return {errno, std::system_category()};
}

// RAII guard around a file descriptor.
class UniqueFd {
  int m_fd = -1;

 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : m_fd(fd) {}
  ~UniqueFd() { reset(); }

  void reset(int fd = -1) {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
    m_fd = fd;
  }

  [[nodiscard]] int get() const { return m_fd; }
  explicit operator bool() const { return m_fd >= 0; }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&&) = delete;
  UniqueFd& operator=(UniqueFd&&) = delete;
};

// Resolves @a mountpoint to a stable canonical form so that a relative path, an
// absolute path and any symlinked form all hash the same. The mount root exists
// while it is served, so canonical() resolves it; otherwise it falls back to a
// lexical absolute path.
std::string canonical_key(const std::filesystem::path& mountpoint) {
  std::error_code error;
  std::filesystem::path resolved =
      std::filesystem::canonical(mountpoint, error);
  if (error) {
    resolved = std::filesystem::absolute(mountpoint).lexically_normal();
  }
  return resolved.string();
}

// True if @a dir is a real directory this user owns with no group or other
// access (0700-style). Uses lstat, so a symlink is rejected, not followed.
bool is_private_dir(const std::filesystem::path& dir) {
  struct stat info {};
  if (::lstat(dir.c_str(), &info) != 0) {
    return false;
  }
  return S_ISDIR(info.st_mode) && info.st_uid == ::getuid() &&
         (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

// Creates @a dir 0700 if absent, then checks it is private to this user.
bool ensure_private_dir(const std::filesystem::path& dir) {
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    return false;
  }
  return is_private_dir(dir);
}

// The per-user runtime directory: $XDG_RUNTIME_DIR when set, otherwise the
// /run/user/<uid> that systemd points it at - derived so mount and unmount
// agree even when one runs without the variable (sudo, cron, a non-login
// shell). nullopt unless it exists and is private to this user. There is
// deliberately no world-writable fallback: a predictable name under /tmp could
// be pre-created by another user, so a missing runtime directory disables the
// channel instead.
std::optional<std::filesystem::path> runtime_directory() {
  std::filesystem::path dir;
  if (const char* xdg = std::getenv("XDG_RUNTIME_DIR");
      xdg != nullptr && xdg[0] != '\0') {
    dir = xdg;
  } else {
    dir = "/run/user/" + std::to_string(::getuid());
  }
  if (!is_private_dir(dir)) {
    return std::nullopt;
  }
  return dir;
}

// The directory holding control sockets: a private makebelieve/ under the
// runtime directory, created if absent. nullopt when there is no usable runtime
// directory, in which case the control channel is unavailable.
std::optional<std::filesystem::path> endpoint_directory() {
  return runtime_directory().and_then(
      [](const std::filesystem::path& runtime)
          -> std::optional<std::filesystem::path> {
        std::filesystem::path dir = runtime / "makebelieve";
        if (!ensure_private_dir(dir)) {
          return std::nullopt;
        }
        return dir;
      });
}

// The control socket's path for an endpoint id, or nullopt when no control
// directory is available.
std::optional<std::filesystem::path> socket_path(const std::string& id) {
  return endpoint_directory().transform(
      [&id](const std::filesystem::path& dir) { return dir / id; });
}

// Fills @a addr for @a path, returning false if the path is too long for
// sun_path.
bool make_address(const std::filesystem::path& path, sockaddr_un& addr) {
  const std::string text = path.string();
  if (text.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, text.c_str(), text.size() + 1);
  return true;
}

}  // namespace

class UnmountChannel::Impl {
  std::stop_source m_source;
  std::filesystem::path m_path;  // The socket file, unlinked on teardown.
  UniqueFd m_listen;
  UniqueFd m_quit;  // Signalled to stop the listener.
  std::thread m_listener;

 public:
  explicit Impl(const std::filesystem::path& mountpoint) {
    const std::optional<std::filesystem::path> path =
        socket_path(endpoint_id(canonical_key(mountpoint)));
    if (!path) {
      // No per-user runtime directory: serve without a control endpoint. The
      // mount still works; only a console interrupt can stop it.
      std::println(
          stderr,
          "makebelieve: no per-user runtime directory for a control "
          "endpoint; `makebelieve unmount` cannot stop this mount, use "
          "Ctrl+C instead");
      return;
    }
    m_path = *path;

    sockaddr_un addr{};
    if (!make_address(m_path, addr)) {
      throw std::runtime_error("mountpoint control path is too long: " +
                               m_path.string());
    }

    m_listen.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!m_listen) {
      throw std::system_error(last_error_code(), "socket");
    }

    bind_endpoint(addr);

    if (::listen(m_listen.get(), 1) != 0) {
      throw std::system_error(last_error_code(), "listen");
    }

    m_quit.reset(::eventfd(0, EFD_CLOEXEC));
    if (!m_quit) {
      throw std::system_error(last_error_code(), "eventfd");
    }

    m_listener = std::thread([this] { listen_loop(); });
  }

  ~Impl() {
    if (!m_listener.joinable()) {
      return;  // Disabled: no endpoint was created.
    }
    const std::uint64_t one = 1;
    const ssize_t written = ::write(m_quit.get(), &one, sizeof(one));
    static_cast<void>(written);
    m_listener.join();
    // Remove the endpoint; the directory is left in place.
    ::unlink(m_path.c_str());
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] std::stop_token token() const { return m_source.get_token(); }

 private:
  // Binds the socket, enforcing single ownership: a bind clash means either a
  // live owner (a connect succeeds - refuse) or a stale socket left by a
  // hard-killed daemon (a connect is refused - unlink and retry).
  void bind_endpoint(const sockaddr_un& addr) {
    if (::bind(m_listen.get(), reinterpret_cast<const sockaddr*>(&addr),
               sizeof(addr)) == 0) {
      return;
    }
    if (errno != EADDRINUSE) {
      throw std::system_error(last_error_code(), "bind");
    }

    const UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (probe &&
        ::connect(probe.get(), reinterpret_cast<const sockaddr*>(&addr),
                  sizeof(addr)) == 0) {
      throw std::runtime_error(
          "another makebelieve instance is already serving this mountpoint");
    }

    // Nobody is listening: the socket file is stale. Replace it.
    if (::unlink(m_path.c_str()) != 0 && errno != ENOENT) {
      throw std::system_error(last_error_code(), "unlink");
    }
    if (::bind(m_listen.get(), reinterpret_cast<const sockaddr*>(&addr),
               sizeof(addr)) != 0) {
      throw std::system_error(last_error_code(), "bind");
    }
  }

  // Accepts a poke (a client connect) into a stop request, until the quit
  // eventfd is signalled.
  void listen_loop() {
    std::array<pollfd, 2> fds{
        pollfd{.fd = m_listen.get(), .events = POLLIN, .revents = 0},
        pollfd{.fd = m_quit.get(), .events = POLLIN, .revents = 0},
    };
    while (true) {
      if (::poll(fds.data(), fds.size(), -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }
      if ((fds[1].revents & POLLIN) != 0) {
        return;  // The quit signal.
      }
      if ((fds[0].revents & POLLIN) != 0) {
        const int client = ::accept(m_listen.get(), nullptr, nullptr);
        if (client >= 0) {
          ::close(client);
          m_source.request_stop();  // Idempotent; keep listening for quit.
        }
      }
    }
  }
};

UnmountChannel::Result UnmountChannel::request_unmount(
    const std::filesystem::path& mountpoint) {
  const std::optional<std::filesystem::path> path =
      socket_path(endpoint_id(canonical_key(mountpoint)));
  sockaddr_un addr{};
  if (!path || !make_address(*path, addr)) {
    return Result::not_mounted;
  }

  const UniqueFd sock(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!sock) {
    throw std::system_error(last_error_code(), "socket");
  }

  // Connecting is the poke. A missing socket (ENOENT) or one nobody is
  // listening on (ECONNREFUSED, a stale file) means nothing is serving the
  // mount.
  if (::connect(sock.get(), reinterpret_cast<const sockaddr*>(&addr),
                sizeof(addr)) != 0) {
    if (errno == ENOENT || errno == ECONNREFUSED) {
      return Result::not_mounted;
    }
    throw std::system_error(last_error_code(), "connect");
  }

  return wait_until_gone(mountpoint) ? Result::ok : Result::teardown_timeout;
}

UnmountChannel::UnmountChannel(const std::filesystem::path& mountpoint)
    : m_impl(std::make_unique<Impl>(mountpoint)) {}

UnmountChannel::~UnmountChannel() = default;

std::stop_token UnmountChannel::token() const {
  return m_impl->token();
}

}  // namespace makebelieve
