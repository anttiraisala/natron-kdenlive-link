// net.h - minimal POSIX socket layer: Unix domain sockets and loopback TCP
// behind one address string.
//
// Address syntax:   tcp:127.0.0.1:47801     or     unix:/path/to/file.sock
//
// Why both: Unix sockets are fast and permission controlled, but sandboxed
// apps (snap, flatpak) cannot see each other's socket files. Loopback TCP is
// reachable from every sandbox that has network access. Because any local
// process can connect to a TCP port, every connection also does a token
// handshake (see client.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nkb {

// RAII owner of a file descriptor. Move only.
class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  Socket& operator=(Socket&& o) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket();
  int fd() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void close();
 private:
  int fd_ = -1;
};

struct Address {
  enum class Kind { Tcp, Unix } kind = Kind::Tcp;
  std::string host;  // Tcp only; must be 127.x.x.x or "localhost"
  uint16_t port = 0;
  std::string path;  // Unix only
  static bool parse(const std::string& text, Address* out, std::string* err);
  std::string str() const;
};

// Creates a listening socket. For Unix sockets a stale file is removed (but
// not if another process still answers on it) and the socket is chmod 0600.
Socket listen_on(const Address& a, std::string* err);
// Waits up to timeout_ms for a connection. Returns an invalid Socket on timeout.
Socket accept_on(int listen_fd, int timeout_ms);
Socket connect_to(const Address& a, int timeout_ms, std::string* err);

// Blocking send of the whole buffer (MSG_NOSIGNAL). extra_flags may be MSG_MORE.
bool send_all(int fd, const void* buf, size_t n, int extra_flags, std::string* err);
// See recv_message in protocol.h for the timeout semantics.
// Returns 0 = Ok, 1 = Timeout, 2 = Closed, 3 = Error.
int recv_all(int fd, void* buf, size_t n, int idle_timeout_ms, std::string* err);

// Seconds a started message may stall before it is treated as broken.
constexpr int kStallTimeoutMs = 10000;

}  // namespace nkb
