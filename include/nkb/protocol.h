// protocol.h - wire protocol shared by the daemon, the Natron worker, the MLT
// filter (milestone 2) and all command line tools.
//
// DESIGN NOTES
//  * Every message is a fixed 104 byte Header followed by `payload_size` bytes.
//  * All integers are little endian (the host is little endian on every
//    platform we target; a static_assert-style check lives in protocol.cpp).
//  * Frames are sent raw (no compression). The header carries geometry, pixel
//    format, alpha mode and colorspace, so other resolutions and formats need
//    no protocol change later.
//  * This header has no dependency on spdlog so the MLT filter can use it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nkb {

// Bytes on the wire read "BNK1". Used to detect garbage or a wrong peer.
constexpr uint32_t kMagic = 0x314B4E42u;
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kCompIdLen = 32;
constexpr uint32_t kMaxNtpLen = 4096;  // longest .ntp path accepted in a FrameRequest/Job

// timeout_ms value meaning "use the daemon's default_request_timeout_ms".
constexpr uint32_t kTimeoutDefault = 0xFFFFFFFFu;

// ---------------------------------------------------------------- enums ----
enum class MsgType : uint16_t {
  Hello = 1,         // client -> daemon. flags = Role, payload = auth token
  HelloAck = 2,      // daemon -> client. status Ok/Error, payload = text
  FrameRequest = 3,  // filter -> daemon. payload = input frame (or empty, see kCacheOnly)
  FrameResult = 4,   // daemon -> filter. payload = image only if status is Ok/CacheHit
  Job = 5,           // daemon -> worker. payload = input frame
  JobResult = 6,     // worker -> daemon. payload = processed frame
  Control = 7,       // tool -> daemon. payload = text command
  ControlReply = 8,  // daemon -> tool. payload = text reply (key=value lines)
};

enum class Role : uint16_t { Filter = 1, Worker = 2, Tool = 3 };

// Outcome of a request. Only Ok and CacheHit carry an image payload; for every
// other status the payload (if any) is a UTF-8 explanation for the logs, and
// the filter must pass its own frame through unchanged.
enum class Status : uint16_t {
  Ok = 0,           // rendered by a worker for this request
  CacheHit = 1,     // served from the output cache
  Miss = 2,         // cache-only request, nothing cached
  Passthrough = 3,  // job accepted/queued, no result within the wait time (timeout 0)
  Timeout = 4,      // waited the full timeout, result not ready (job keeps running)
  Skipped = 5,      // job dropped from the input queue (buffer_skip)
  Error = 6,        // protocol or worker error, see payload text
  Rejected = 7,     // queue full and policy is show_cached
};

enum class PixelFormat : uint16_t { Unknown = 0, RGBA8 = 1, RGBA16 = 2, RGBAF32 = 3 };
enum class AlphaMode : uint8_t { Straight = 0, Premultiplied = 1 };
enum class Colorspace : uint8_t { Unspecified = 0, SRGB = 1, Rec709 = 2, LinearSRGB = 3 };

namespace flags {
// FrameRequest: only look in the cache. No payload is sent; the reply is
// CacheHit (with image) or Miss. Avoids sending a whole frame on a hit.
constexpr uint16_t kCacheOnly = 1u << 0;
}  // namespace flags

size_t bytes_per_pixel(PixelFormat f);
const char* to_string(MsgType t);
const char* to_string(Status s);
const char* to_string(Role r);

// -------------------------------------------------------------- header -----
#pragma pack(push, 1)
struct Header {
  uint32_t magic;          //  0  kMagic
  uint16_t version;        //  4  kProtocolVersion
  uint16_t type;           //  6  MsgType
  uint64_t request_id;     //  8  echoed by the daemon in the reply
  int64_t frame_number;    // 16  timeline frame (informational + logging)
  uint32_t width;          // 24
  uint32_t height;         // 28
  uint16_t pixel_format;   // 32  PixelFormat
  uint8_t alpha_mode;      // 34  AlphaMode
  uint8_t colorspace;      // 35  Colorspace
  uint16_t status;         // 36  Status (replies)
  uint16_t flags;          // 38  request flags; Role in Hello
  uint32_t timeout_ms;     // 40  how long the daemon may block the requester
  uint32_t ntp_len;        // 44  FrameRequest/Job: bytes of a .ntp path that follow the image
                           //     in the payload (0 = none: the worker uses comps/<comp_id>.ntp)
  uint64_t key_hi;         // 48  content hash of everything the result depends on
  uint64_t key_lo;         // 56
  uint64_t payload_size;   // 64  bytes following the header
  char comp_id[kCompIdLen];// 72  composition id, NUL padded (logging only)
};
#pragma pack(pop)
static_assert(sizeof(Header) == 104, "Header layout changed: bump kProtocolVersion");

Header make_header(MsgType t);
void set_comp_id(Header& h, std::string_view id);
std::string get_comp_id(const Header& h);

// Computes the payload size implied by the geometry. Returns false if the
// geometry/format is invalid or larger than max_bytes (protects against
// absurd allocations from a corrupt or hostile peer).
bool expected_payload_bytes(const Header& h, uint64_t max_bytes, uint64_t* out);

// ----------------------------------------------------------------- key -----
// 128 bit content key. The producer of a request (the filter) builds it from
// everything the output depends on: input identity, the composition file hash,
// parameter values, frame number and, for nested comps, the upstream key.
// The daemon treats it as opaque.
struct Key {
  uint64_t hi = 0;
  uint64_t lo = 0;
  bool operator==(const Key& o) const { return hi == o.hi && lo == o.lo; }
  std::string hex() const;
};
struct KeyHash {
  size_t operator()(const Key& k) const { return static_cast<size_t>(k.hi ^ (k.lo * 0x9E3779B97F4A7C15ull)); }
};

class KeyBuilder {
 public:
  KeyBuilder& add(std::string_view s);  // length-prefixed, so ("ab","c") != ("a","bc")
  KeyBuilder& add_u64(uint64_t v);
  KeyBuilder& add_key(const Key& k);    // chain an upstream composition's key
  // Hashes a large buffer (pixels) several GB/s. Length-prefixed like add().
  KeyBuilder& add_blob(const void* p, size_t n);
  Key finish() const;
 private:
  void add_bytes(const void* p, size_t n);
  uint64_t a_ = 0xcbf29ce484222325ull;
  uint64_t b_ = 0x9E3779B97F4A7C15ull;
};

// ------------------------------------------------------------- messages ----
struct Message {
  Header h{};
  std::vector<uint8_t> payload;
};

enum class RecvResult { Ok, Timeout, Closed, Error };

// Sends header + payload. Fills magic/version/payload_size itself.
bool send_message(int fd, Header h, const void* payload, size_t n, std::string* err);
// Same, with a second buffer sent right after the first (image + .ntp path, without copying the image).
bool send_message(int fd, Header h, const void* payload, size_t n, const void* tail, size_t tail_n, std::string* err);
// The .ntp path at the end of a FrameRequest/Job payload ("" when ntp_len is 0 or does not fit).
std::string payload_ntp_path(const Header& h, const std::vector<uint8_t>& payload);
// Receives one message. Waits up to idle_timeout_ms for the first byte
// (returns Timeout if nothing arrives); once a message has started, the rest
// must arrive within a fixed stall timeout or the result is Error.
RecvResult recv_message(int fd, Message& m, int idle_timeout_ms, uint64_t max_payload, std::string* err);

}  // namespace nkb
