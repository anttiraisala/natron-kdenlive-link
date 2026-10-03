#include "nkb/protocol.h"

#include <cstdio>
#include <cstring>

#include <sys/socket.h>

#include "nkb/net.h"

namespace nkb {

// The wire format is little endian and we copy structs directly.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "big endian hosts are not supported");

size_t bytes_per_pixel(PixelFormat f) {
  switch (f) {
    case PixelFormat::RGBA8: return 4;
    case PixelFormat::RGBA16: return 8;
    case PixelFormat::RGBAF32: return 16;
    default: return 0;
  }
}

const char* to_string(MsgType t) {
  switch (t) {
    case MsgType::Hello: return "Hello";
    case MsgType::HelloAck: return "HelloAck";
    case MsgType::FrameRequest: return "FrameRequest";
    case MsgType::FrameResult: return "FrameResult";
    case MsgType::Job: return "Job";
    case MsgType::JobResult: return "JobResult";
    case MsgType::Control: return "Control";
    case MsgType::ControlReply: return "ControlReply";
  }
  return "Unknown";
}

const char* to_string(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::CacheHit: return "cache_hit";
    case Status::Miss: return "miss";
    case Status::Passthrough: return "passthrough";
    case Status::Timeout: return "timeout";
    case Status::Skipped: return "skipped";
    case Status::Error: return "error";
    case Status::Rejected: return "rejected";
  }
  return "unknown";
}

const char* to_string(Role r) {
  switch (r) {
    case Role::Filter: return "filter";
    case Role::Worker: return "worker";
    case Role::Tool: return "tool";
  }
  return "unknown";
}

Header make_header(MsgType t) {
  Header h;
  std::memset(&h, 0, sizeof h);
  h.magic = kMagic;
  h.version = kProtocolVersion;
  h.type = static_cast<uint16_t>(t);
  h.timeout_ms = kTimeoutDefault;
  return h;
}

void set_comp_id(Header& h, std::string_view id) {
  std::memset(h.comp_id, 0, kCompIdLen);
  std::memcpy(h.comp_id, id.data(), id.size() < kCompIdLen ? id.size() : kCompIdLen - 1);
}

std::string get_comp_id(const Header& h) {
  size_t n = 0;
  while (n < kCompIdLen && h.comp_id[n] != '\0') ++n;
  return std::string(h.comp_id, n);
}

bool expected_payload_bytes(const Header& h, uint64_t max_bytes, uint64_t* out) {
  const size_t bpp = bytes_per_pixel(static_cast<PixelFormat>(h.pixel_format));
  if (bpp == 0 || h.width == 0 || h.height == 0) return false;
  const uint64_t px = static_cast<uint64_t>(h.width) * h.height;  // cannot overflow 64 bit
  if (px > max_bytes / bpp) return false;
  *out = px * bpp;
  return true;
}

// ---------------------------------------------------------------- Key ------
static uint64_t splitmix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

std::string Key::hex() const {
  char buf[33];
  std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(hi),
                static_cast<unsigned long long>(lo));
  return buf;
}

void KeyBuilder::add_bytes(const void* p, size_t n) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) {
    a_ = (a_ ^ b[i]) * 0x100000001b3ull;      // FNV-1a
    b_ = splitmix(b_ ^ (b[i] + 0x100ull));     // independent second lane
  }
}

KeyBuilder& KeyBuilder::add(std::string_view s) {
  add_u64(s.size());  // length prefix removes concatenation ambiguity
  add_bytes(s.data(), s.size());
  return *this;
}

KeyBuilder& KeyBuilder::add_u64(uint64_t v) {
  add_bytes(&v, sizeof v);
  return *this;
}

KeyBuilder& KeyBuilder::add_blob(const void* p, size_t n) {
  add_u64(n);
  const auto* b = static_cast<const uint8_t*>(p);
  uint64_t x1 = a_, x2 = b_;
  size_t i = 0;
  // Two independent multiply/xor-shift lanes over 8 byte words: fast enough to
  // hash a 1080p RGBA frame (8 MB) in a few milliseconds.
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, b + i, 8);
    x1 = (x1 ^ w) * 0x9E3779B97F4A7C15ull;
    x1 ^= x1 >> 32;
    x2 = (x2 + w) * 0xC2B2AE3D27D4EB4Full;
    x2 = ((x2 << 31) | (x2 >> 33)) ^ x1;
  }
  a_ = x1;
  b_ = x2;
  add_bytes(b + i, n - i);  // 0..7 tail bytes
  return *this;
}

KeyBuilder& KeyBuilder::add_key(const Key& k) {
  add_u64(k.hi);
  add_u64(k.lo);
  return *this;
}

Key KeyBuilder::finish() const {
  Key k;
  k.hi = splitmix(a_ ^ 0x5851F42D4C957F2Dull);
  k.lo = splitmix(b_ ^ a_);
  return k;
}

// ------------------------------------------------------------- messages ----
bool send_message(int fd, Header h, const void* payload, size_t n, std::string* err) {
  h.magic = kMagic;
  h.version = kProtocolVersion;
  h.payload_size = n;
  if (!send_all(fd, &h, sizeof h, n ? MSG_MORE : 0, err)) return false;
  if (n && !send_all(fd, payload, n, 0, err)) return false;
  return true;
}

static RecvResult map_result(int r) {
  switch (r) {
    case 0: return RecvResult::Ok;
    case 1: return RecvResult::Timeout;
    case 2: return RecvResult::Closed;
    default: return RecvResult::Error;
  }
}

RecvResult recv_message(int fd, Message& m, int idle_timeout_ms, uint64_t max_payload, std::string* err) {
  RecvResult r = map_result(recv_all(fd, &m.h, sizeof m.h, idle_timeout_ms, err));
  if (r != RecvResult::Ok) return r;
  if (m.h.magic != kMagic) {
    if (err) *err = "bad magic (peer is not speaking this protocol)";
    return RecvResult::Error;
  }
  if (m.h.version != kProtocolVersion) {
    if (err) *err = "protocol version mismatch: peer=" + std::to_string(m.h.version) +
                    " local=" + std::to_string(kProtocolVersion);
    return RecvResult::Error;
  }
  if (m.h.payload_size > max_payload) {
    if (err) *err = "payload too large: " + std::to_string(m.h.payload_size);
    return RecvResult::Error;
  }
  m.payload.resize(static_cast<size_t>(m.h.payload_size));
  if (m.h.payload_size) {
    r = map_result(recv_all(fd, m.payload.data(), m.payload.size(), kStallTimeoutMs, err));
    if (r == RecvResult::Timeout || r == RecvResult::Closed) {
      if (err && err->empty()) *err = "connection lost in the middle of a payload";
      return RecvResult::Error;
    }
    if (r != RecvResult::Ok) return r;
  }
  return RecvResult::Ok;
}

}  // namespace nkb
