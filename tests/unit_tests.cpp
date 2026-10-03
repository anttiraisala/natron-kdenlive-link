// Unit tests without an external framework. Each CHECK prints the failing
// expression and the program exits non-zero if any check failed.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/socket.h>
#include <unistd.h>

#include "nkb/cache.h"
#include "nkb/config.h"
#include "nkb/net.h"
#include "nkb/protocol.h"

static int g_failed = 0;
#define CHECK(expr)                                                         \
  do {                                                                      \
    if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failed; } \
  } while (0)

using namespace nkb;

static std::shared_ptr<const Frame> frame_of(size_t bytes) {
  auto f = std::make_shared<Frame>();
  f->data.assign(bytes, 7);
  return f;
}

static void test_key() {
  Key a = KeyBuilder().add("ab").add("c").finish();
  Key b = KeyBuilder().add("a").add("bc").finish();
  Key c = KeyBuilder().add("ab").add("c").finish();
  CHECK(!(a == b));  // length prefix prevents concatenation collisions
  CHECK(a == c);     // deterministic
  CHECK(a.hex().size() == 32);
  Key up = KeyBuilder().add("comp1").add_u64(5).finish();
  Key n1 = KeyBuilder().add("comp2").add_key(up).add_u64(5).finish();
  Key up2 = KeyBuilder().add("comp1").add_u64(6).finish();  // upstream changed
  Key n2 = KeyBuilder().add("comp2").add_key(up2).add_u64(5).finish();
  CHECK(!(n1 == n2));  // nested key changes when the upstream key changes
}

static void test_blob_hash() {
  std::vector<uint8_t> a(1000, 5), b = a;
  Key ka = KeyBuilder().add_blob(a.data(), a.size()).finish();
  CHECK(ka == KeyBuilder().add_blob(b.data(), b.size()).finish());
  for (size_t pos : {size_t(0), size_t(7), size_t(8), size_t(999)}) {  // word and tail positions
    b = a;
    b[pos] ^= 1;
    CHECK(!(ka == KeyBuilder().add_blob(b.data(), b.size()).finish()));
  }
  CHECK(!(ka == KeyBuilder().add_blob(a.data(), 999).finish()));  // length matters
}

static void test_header() {
  CHECK(sizeof(Header) == 104);
  Header h = make_header(MsgType::FrameRequest);
  h.width = 1920; h.height = 1080;
  h.pixel_format = static_cast<uint16_t>(PixelFormat::RGBA8);
  uint64_t n = 0;
  CHECK(expected_payload_bytes(h, 1ull << 30, &n) && n == 1920ull * 1080 * 4);
  h.pixel_format = static_cast<uint16_t>(PixelFormat::RGBAF32);
  CHECK(expected_payload_bytes(h, 1ull << 30, &n) && n == 1920ull * 1080 * 16);
  h.width = 0xFFFFFFFFu; h.height = 0xFFFFFFFFu;  // hostile geometry must not overflow
  CHECK(!expected_payload_bytes(h, 1ull << 30, &n));
  h.pixel_format = 99;
  h.width = 10; h.height = 10;
  CHECK(!expected_payload_bytes(h, 1ull << 30, &n));
  set_comp_id(h, "a-very-long-composition-identifier-that-exceeds-the-field");
  CHECK(get_comp_id(h).size() == kCompIdLen - 1);
}

static void test_cache() {
  FrameCache c(100);
  Key k1{1, 1}, k2{2, 2}, k3{3, 3}, k4{4, 4};
  c.put(k1, frame_of(40));
  c.put(k2, frame_of(40));
  CHECK(c.get(k1) != nullptr);          // k1 becomes most recently used
  CHECK(c.put(k3, frame_of(40)) == 1);  // evicts k2 (least recently used)
  CHECK(c.get(k2) == nullptr);
  CHECK(c.get(k1) != nullptr && c.get(k3) != nullptr);
  c.put(k4, frame_of(500));             // bigger than the whole budget
  CHECK(c.get(k4) == nullptr);
  CHECK(c.stats().too_large == 1);
  c.put(k1, frame_of(10));              // replace shrinks the byte count
  CHECK(c.stats().bytes == 50);
  auto r = c.clear();
  CHECK(r.entries == 2 && r.bytes == 50);
  CHECK(c.stats().entries == 0 && c.stats().bytes == 0);
}

static void test_config() {
  Config c;
  CHECK(c.get_int("daemon", "cache_memory_mb") == 1024);
  CHECK(c.get_int("daemon", "cache_memory_max_mb") == 5120);
  CHECK(c.get("daemon", "buffer_behavior") == "buffer_skip");
  std::vector<std::string> errs;
  CHECK(c.validate(&errs));
  c.set("daemon", "cache_memory_mb", "abc");
  CHECK(!c.validate(&errs));
  c.set("daemon", "cache_memory_mb", "10");
  c.set("daemon", "buffer_behavior", "bogus");
  errs.clear();
  CHECK(!c.validate(&errs) && errs.size() == 1);

  const char* path = "/tmp/nkb_unit_config.ini";
  { std::ofstream o(path); o << "[daemon]\nbuffer_behavior = \"pause\"\n# c\nnatron_timeout_seconds=3\nbogus_key=1\n[logging]\nlevel = info\n"; }
  Config d;
  std::vector<std::string> warn;
  CHECK(d.load(path, &warn));
  CHECK(d.get("daemon", "buffer_behavior") == "pause");  // quotes stripped
  CHECK(d.get_int("daemon", "natron_timeout_seconds") == 3);
  CHECK(d.get("logging", "level") == "info");
  CHECK(warn.size() == 1);  // unknown key reported
  unlink(path);

  // The generated default file must parse back into the default values.
  { std::ofstream o(path); o << Config::default_file_text(); }
  Config e;
  warn.clear();
  CHECK(e.load(path, &warn) && warn.empty() && e.validate(nullptr));
  CHECK(e.get_int("daemon", "input_queue_memory_mb") == 512);
  unlink(path);
}

static void test_address() {
  Address a;
  std::string err;
  CHECK(Address::parse("tcp:127.0.0.1:47801", &a, &err) && a.port == 47801);
  CHECK(Address::parse("tcp:localhost:5000", &a, &err) && a.host == "127.0.0.1");
  CHECK(!Address::parse("tcp:0.0.0.0:5000", &a, &err));   // not loopback
  CHECK(!Address::parse("tcp:192.168.1.5:5000", &a, &err));
  CHECK(!Address::parse("tcp:127.0.0.1:99999", &a, &err));
  CHECK(Address::parse("unix:/tmp/x.sock", &a, &err) && a.path == "/tmp/x.sock");
  CHECK(!Address::parse("http://x", &a, &err));
}

static void test_message_roundtrip() {
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  Header h = make_header(MsgType::FrameRequest);
  h.frame_number = 42; h.width = 4; h.height = 4;
  std::vector<uint8_t> data(64);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);
  std::string err;
  CHECK(send_message(sv[0], h, data.data(), data.size(), &err));
  Message m;
  CHECK(recv_message(sv[1], m, 1000, 1 << 20, &err) == RecvResult::Ok);
  CHECK(m.h.frame_number == 42 && m.payload == data);
  // nothing pending: idle timeout
  CHECK(recv_message(sv[1], m, 50, 1 << 20, &err) == RecvResult::Timeout);
  // payload above the limit is refused
  CHECK(send_message(sv[0], h, data.data(), data.size(), &err));
  CHECK(recv_message(sv[1], m, 1000, 10, &err) == RecvResult::Error);
  // garbage instead of a header
  char junk[104];
  for (char& c : junk) c = 'x';
  CHECK(send_all(sv[0], junk, sizeof junk, 0, &err));
  CHECK(recv_message(sv[1], m, 1000, 1 << 20, &err) == RecvResult::Error);
  close(sv[0]);
  close(sv[1]);
  // After an Error the stream is out of sync and the caller closes it, so the
  // clean-close check uses a fresh pair.
  int sv2[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);
  close(sv2[0]);
  CHECK(recv_message(sv2[1], m, 1000, 1 << 20, &err) == RecvResult::Closed);
  close(sv2[1]);
}

int main() {
  test_key();
  test_blob_hash();
  test_header();
  test_cache();
  test_config();
  test_address();
  test_message_roundtrip();
  if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
  std::puts("all unit tests passed");
  return 0;
}
