// SPDX-License-Identifier: MIT
#include "unmountchannel.hpp"

#include "iocontext.hpp"

#include <stdexec/execution.hpp>

#include <cassert>
#include <cerrno>
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
#include <utility>
#include <vector>

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
  std::stop_source m_source;     // Shares the channel's stop state.
  std::filesystem::path m_path;  // The socket file, unlinked on teardown.
  UniqueFd m_listen;

  // Every client that has poked, held open until teardown: `request_unmount`
  // waits for its end to close. Only touched from the IoContext's thread
  // until m_listening is joined.
  std::vector<int> m_clients;

  // The wait on m_listen, stopped and joined on destruction.
  stdexec::counting_scope m_listening;
  bool m_serving = false;

 public:
  Impl(const std::filesystem::path& mountpoint,
       IoContext& io,
       std::stop_source source)
      : m_source(std::move(source)) {
    const std::optional<std::filesystem::path> path =
        socket_path(endpoint_id(canonical_key(mountpoint)));
    if (!path) {
      // No per-user runtime directory: serve without a control endpoint. The
      // mount still works; only a console interrupt can stop it.
      std::println(
          stderr,
          "makebelieve: no private per-user runtime directory for a control "
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

    // Non-blocking, so an accept for a client that has already gone again
    // returns instead of stalling the context's thread.
    m_listen.reset(
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!m_listen) {
      throw std::system_error(last_error_code(), "socket");
    }

    bind_endpoint(addr);

    // Room for as many requests as arrive at once, as one left waiting to
    // connect when the endpoint closes is told nothing is mounted.
    if (::listen(m_listen.get(), SOMAXCONN) != 0) {
      throw std::system_error(last_error_code(), "listen");
    }

    io.spawn_watch(
        m_listen.get(), m_listening, [this]() noexcept { accept_pokes(); },
        [](std::error_code error) noexcept {
          // Best-effort, as this runs on the IoContext's thread.
          try {
            std::println(
                stderr,
                "makebelieve: stopped watching for unmount requests: {}",
                error.message());
          } catch (...) {  // NOLINT(bugprone-empty-catch)
          }
        });
    m_serving = true;
  }

  ~Impl() {
    if (!m_serving) {
      return;  // Disabled: no endpoint was created.
    }
    m_listening.request_stop();
    stdexec::sync_wait(m_listening.join());
    // Remove the endpoint; the directory is left in place. Only then let the
    // clients go, so a waiting `request_unmount` returns once nothing of this
    // mount is left.
    ::unlink(m_path.c_str());
    m_listen.reset();
    for (const int client : m_clients) {
      ::close(client);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

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

  // Turns every poke (a client connect) waiting to be accepted into a stop
  // request, keeping the client to release at teardown. Idempotent, so the
  // watch keeps going to the end.
  void accept_pokes() noexcept {
    while (true) {
      const int client =
          ::accept4(m_listen.get(), nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;  // Drained.
        }
        if (errno == EINTR || errno == ECONNABORTED) {
          continue;  // Interrupted, or that client gave up first.
        }
        // A client is waiting but cannot be accepted - out of descriptors,
        // say. The socket being readable is its poke all the same, so stop:
        // it is let go, with a reset, when the socket closes at teardown.
        // Until then the watch keeps reporting the socket ready.
        m_source.request_stop();
        return;
      }
      try {
        m_clients.push_back(client);
      } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Out of memory: leave it open, untracked. Closing it now would let
        // that `request_unmount` return before teardown, whereas the process
        // exiting closes it at the right time.
      }
      m_source.request_stop();
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

  // The mount holds its end open until it has torn down, or its process
  // exits, so wait for that end to close: a read of 0 bytes, or a reset.
  // The socket blocks, so each read waits for that.
  while (true) {
    char byte = 0;
    const ssize_t got = ::recv(sock.get(), &byte, 1, 0);
    if (got == 0 || (got < 0 && errno == ECONNRESET)) {
      return Result::ok;
    }
    if (got < 0 && errno != EINTR) {
      throw std::system_error(last_error_code(), "recv");
    }
    // The mount never writes, but anything that arrives is ignored.
  }
}

UnmountChannel::UnmountChannel() = default;

UnmountChannel::~UnmountChannel() = default;

void UnmountChannel::listen(const std::filesystem::path& mountpoint,
                            IoContext& io) {
  assert(m_impl == nullptr && "UnmountChannel is already listening");
  m_impl = std::make_unique<Impl>(mountpoint, io, m_source);
}

}  // namespace makebelieve
