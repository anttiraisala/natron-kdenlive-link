#include "nkb/net.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace nkb {

static std::string errno_text(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

Socket& Socket::operator=(Socket&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    o.fd_ = -1;
  }
  return *this;
}
Socket::~Socket() { close(); }
void Socket::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

// -------------------------------------------------------------- Address ----
bool Address::parse(const std::string& text, Address* out, std::string* err) {
  Address a;
  if (text.rfind("unix:", 0) == 0) {
    a.kind = Kind::Unix;
    a.path = text.substr(5);
    if (a.path.empty() || a.path.size() >= sizeof(sockaddr_un::sun_path)) {
      if (err) *err = "unix socket path empty or too long (max " +
                      std::to_string(sizeof(sockaddr_un::sun_path) - 1) + "): " + text;
      return false;
    }
  } else if (text.rfind("tcp:", 0) == 0) {
    a.kind = Kind::Tcp;
    const std::string rest = text.substr(4);
    const size_t colon = rest.rfind(':');
    if (colon == std::string::npos) {
      if (err) *err = "tcp address needs host:port : " + text;
      return false;
    }
    a.host = rest.substr(0, colon);
    if (a.host == "localhost") a.host = "127.0.0.1";
    const long port = std::strtol(rest.c_str() + colon + 1, nullptr, 10);
    if (port < 1 || port > 65535) {
      if (err) *err = "tcp port out of range: " + text;
      return false;
    }
    a.port = static_cast<uint16_t>(port);
    // Security: frames and the auth token must never leave this machine.
    if (a.host.rfind("127.", 0) != 0) {
      if (err) *err = "only loopback (127.x.x.x / localhost) tcp addresses are allowed: " + text;
      return false;
    }
  } else {
    if (err) *err = "address must start with tcp: or unix: : " + text;
    return false;
  }
  *out = a;
  return true;
}

std::string Address::str() const {
  return kind == Kind::Unix ? "unix:" + path : "tcp:" + host + ":" + std::to_string(port);
}

// ------------------------------------------------------------- sockets -----
static void tune(int fd, const Address& a) {
  if (a.kind == Address::Kind::Tcp) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // frames are latency sensitive
  }
  timeval tv{};
  tv.tv_sec = kStallTimeoutMs / 1000;  // a stuck peer must not block a sender forever
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static bool fill_addr(const Address& a, sockaddr_storage* ss, socklen_t* len) {
  std::memset(ss, 0, sizeof *ss);
  if (a.kind == Address::Kind::Unix) {
    auto* un = reinterpret_cast<sockaddr_un*>(ss);
    un->sun_family = AF_UNIX;
    std::memcpy(un->sun_path, a.path.c_str(), a.path.size() + 1);
    *len = sizeof(sockaddr_un);
  } else {
    auto* in = reinterpret_cast<sockaddr_in*>(ss);
    in->sin_family = AF_INET;
    in->sin_port = htons(a.port);
    if (inet_pton(AF_INET, a.host.c_str(), &in->sin_addr) != 1) return false;
    *len = sizeof(sockaddr_in);
  }
  return true;
}

Socket listen_on(const Address& a, std::string* err) {
  sockaddr_storage ss;
  socklen_t len;
  if (!fill_addr(a, &ss, &len)) {
    if (err) *err = "invalid address " + a.str();
    return Socket();
  }
  if (a.kind == Address::Kind::Unix) {
    struct stat st;
    if (stat(a.path.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode)) {
        if (err) *err = a.path + " exists and is not a socket";
        return Socket();
      }
      // If someone still answers, a daemon is already running: do not steal it.
      std::string ignored;
      Socket probe = connect_to(a, 200, &ignored);
      if (probe.valid()) {
        if (err) *err = "another process is already listening on " + a.str();
        return Socket();
      }
      unlink(a.path.c_str());  // stale file from a crashed run
    }
  }
  Socket s(socket(a.kind == Address::Kind::Unix ? AF_UNIX : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!s.valid()) {
    if (err) *err = errno_text("socket");
    return Socket();
  }
  if (a.kind == Address::Kind::Tcp) {
    int one = 1;
    setsockopt(s.fd(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  }
  const mode_t old_umask = umask(077);  // socket file created 0600
  const int br = bind(s.fd(), reinterpret_cast<sockaddr*>(&ss), len);
  umask(old_umask);
  if (br != 0) {
    if (err) *err = errno_text(("bind " + a.str()).c_str());
    return Socket();
  }
  if (listen(s.fd(), 16) != 0) {
    if (err) *err = errno_text("listen");
    return Socket();
  }
  return s;
}

Socket accept_on(int listen_fd, int timeout_ms) {
  pollfd p{listen_fd, POLLIN, 0};
  if (poll(&p, 1, timeout_ms) <= 0) return Socket();
  Socket s(accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC));
  if (s.valid()) {
    int one = 1;
    setsockopt(s.fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // ignored for unix sockets
    timeval tv{};
    tv.tv_sec = kStallTimeoutMs / 1000;
    setsockopt(s.fd(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  }
  return s;
}

Socket connect_to(const Address& a, int timeout_ms, std::string* err) {
  sockaddr_storage ss;
  socklen_t len;
  if (!fill_addr(a, &ss, &len)) {
    if (err) *err = "invalid address " + a.str();
    return Socket();
  }
  Socket s(socket(a.kind == Address::Kind::Unix ? AF_UNIX : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!s.valid()) {
    if (err) *err = errno_text("socket");
    return Socket();
  }
  // Non-blocking connect so that timeout_ms is honoured.
  const int fl = fcntl(s.fd(), F_GETFL, 0);
  fcntl(s.fd(), F_SETFL, fl | O_NONBLOCK);
  int r = connect(s.fd(), reinterpret_cast<sockaddr*>(&ss), len);
  if (r != 0 && errno != EINPROGRESS) {
    if (err) *err = errno_text(("connect " + a.str()).c_str());
    return Socket();
  }
  if (r != 0) {
    pollfd p{s.fd(), POLLOUT, 0};
    if (poll(&p, 1, timeout_ms) <= 0) {
      if (err) *err = "connect " + a.str() + ": timed out";
      return Socket();
    }
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    getsockopt(s.fd(), SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr != 0) {
      errno = soerr;
      if (err) *err = errno_text(("connect " + a.str()).c_str());
      return Socket();
    }
  }
  fcntl(s.fd(), F_SETFL, fl);
  tune(s.fd(), a);
  return s;
}

bool send_all(int fd, const void* buf, size_t n, int extra_flags, std::string* err) {
  const auto* p = static_cast<const uint8_t*>(buf);
  size_t sent = 0;
  while (sent < n) {
    const ssize_t k = send(fd, p + sent, n - sent, MSG_NOSIGNAL | extra_flags);
    if (k < 0) {
      if (errno == EINTR) continue;
      if (err) *err = errno_text("send");
      return false;
    }
    sent += static_cast<size_t>(k);
  }
  return true;
}

int recv_all(int fd, void* buf, size_t n, int idle_timeout_ms, std::string* err) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    const int wait = got == 0 ? idle_timeout_ms : kStallTimeoutMs;
    pollfd pfd{fd, POLLIN, 0};
    const int r = poll(&pfd, 1, wait);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (err) *err = errno_text("poll");
      return 3;
    }
    if (r == 0) {
      if (got == 0) return 1;  // idle timeout, nothing received
      if (err) *err = "peer stalled in the middle of a message";
      return 3;
    }
    const ssize_t k = recv(fd, p + got, n - got, 0);
    if (k == 0) {
      if (got == 0) return 2;  // clean close between messages
      if (err) *err = "connection closed in the middle of a message";
      return 3;
    }
    if (k < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      if (err) *err = errno_text("recv");
      return 3;
    }
    got += static_cast<size_t>(k);
  }
  return 0;
}

}  // namespace nkb
