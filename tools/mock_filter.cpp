// nkb-mock-filter - stands in for the MLT filter. Generates deterministic test
// frames, requests them through the daemon the way the real filter will
// (cache-only probe first, full request on a miss), verifies every returned
// pixel and prints one summary line.
//
//   --frames N --start F        frame range (default 10 frames from 0)
//   --width W --height H        default 320x180, RGBA8
//   --comp NAME                 composition id (part of the key)
//   --seed S                    changes the input content, and therefore the keys
//   --timeout-ms T              per request wait; 0 = queue only (playback mode)
//   --expect-transform invert|passthrough|srgb-invert   what the worker should have done
//                               (srgb-invert = invert in linear light: decode sRGB, 1-x, encode)
//   --tolerance N               allowed absolute error per RGB byte (Natron's colour and
//                               premultiply round trip is not bit exact); alpha must be exact
//   --alpha-min A               generated alpha is in [A,255]. Straight-alpha colour at very
//                               low alpha cannot survive a premultiplied pipeline, so tests
//                               against Natron use e.g. 64
//   --expect k=v[,k=v...]       require summary counters, e.g. ok=10,cache_hit=0
//   --token T                   override the token (tests authentication)
//   --address A                 override filter_address
// Exit code 0 only if every pixel matched and all --expect conditions hold.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

#include "common.h"

using namespace nkb;

namespace {

// Deterministic pattern: depends on seed, frame and pixel position.
void fill_frame(std::vector<uint8_t>& px, uint32_t w, uint32_t h, long seed, long frame, int alpha_min) {
  px.resize(static_cast<size_t>(w) * h * 4);
  uint32_t x = static_cast<uint32_t>(seed * 2654435761u + frame * 40503u + 1);
  for (size_t i = 0; i < px.size(); ++i) {
    x = x * 1664525u + 1013904223u;
    px[i] = static_cast<uint8_t>(x >> 24);
    if (i % 4 == 3) px[i] = static_cast<uint8_t>(alpha_min + px[i] % (256 - alpha_min));
  }
}

// Compares a result with the expected transform of the input. RGB may deviate by `tolerance`,
// alpha must be exact. Tracks the largest deviations seen over the whole run.
int g_max_rgb_err = 0, g_max_alpha_err = 0;
double srgb_decode(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double srgb_encode(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055; }

// transform: 0 = passthrough, 1 = invert RGB, 2 = invert RGB in linear light
bool matches(const std::vector<uint8_t>& in, const std::vector<uint8_t>& out, int transform, int tolerance) {
  if (in.size() != out.size()) return false;
  bool ok = true;
  for (size_t i = 0; i < in.size(); ++i) {
    int want = in[i];
    const bool is_alpha = (i % 4) == 3;
    if (transform == 1 && !is_alpha) want = 255 - want;
    if (transform == 2 && !is_alpha) want = static_cast<int>(std::lround(255.0 * srgb_encode(1.0 - srgb_decode(want / 255.0))));
    const int err = std::abs(out[i] - want);
    if (is_alpha) {
      if (err > g_max_alpha_err) g_max_alpha_err = err;
      if (err != 0) ok = false;
    } else {
      if (err > g_max_rgb_err) g_max_rgb_err = err;
      if (err > tolerance) ok = false;
    }
  }
  return ok;
}

std::map<std::string, long> parse_expect(const std::string& s) {
  std::map<std::string, long> m;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    const size_t eq = item.find('=');
    if (eq != std::string::npos) m[item.substr(0, eq)] = std::strtol(item.c_str() + eq + 1, nullptr, 10);
  }
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  nkbtools::Args args(argc, argv);
  nkbtools::Env env;
  std::string err;
  if (!nkbtools::load_env(&env, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  init_logging("mock-filter", env.cfg.get("logging", "level"), nkbtools::log_path(env),
               env.cfg.get_bool("logging", "console"), nullptr);

  Address addr;
  if (!Address::parse(args.str("address", env.cfg.get("daemon", "filter_address")), &addr, &err)) {
    spdlog::error("event=bad_address reason=\"{}\"", err);
    return 1;
  }
  const long frames = args.num("frames", 10), start = args.num("start", 0);
  const uint32_t w = static_cast<uint32_t>(args.num("width", 320)), h = static_cast<uint32_t>(args.num("height", 180));
  const std::string comp = args.str("comp", "comp_test");
  const long seed = args.num("seed", 1);
  const long timeout_ms = args.num("timeout-ms", 2000);
  const std::string tf = args.str("expect-transform", "invert");
  const int transform = tf == "passthrough" ? 0 : tf == "srgb-invert" ? 2 : 1;
  const int tolerance = static_cast<int>(args.num("tolerance", 0));
  const int alpha_min = static_cast<int>(args.num("alpha-min", 0));
  const std::string token = args.has("token") ? args.str("token") : env.token;

  Socket s = connect_and_handshake(addr, Role::Filter, token, 3000, &err);
  if (!s.valid()) {
    spdlog::error("event=connect_failed address={} reason=\"{}\"", addr.str(), err);
    std::printf("summary connect_failed=1\n");
    return 3;
  }
  spdlog::info("event=filter_connected address={} frames={} size={}x{} timeout_ms={}", addr.str(), frames, w, h, timeout_ms);

  std::map<std::string, long> count;
  long mismatches = 0;
  uint64_t bytes_moved = 0;
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> input;
  uint64_t req_id = 1;

  for (long f = start; f < start + frames; ++f) {
    fill_frame(input, w, h, seed, f, alpha_min);
    // The key covers everything the result depends on (here: comp, seed, frame, size).
    const Key key = KeyBuilder().add(comp).add_u64(static_cast<uint64_t>(seed)).add_u64(static_cast<uint64_t>(f))
                        .add_u64(w).add_u64(h).finish();
    Header rq = make_header(MsgType::FrameRequest);
    rq.request_id = req_id++;
    rq.frame_number = f;
    rq.width = w;
    rq.height = h;
    rq.pixel_format = static_cast<uint16_t>(PixelFormat::RGBA8);
    rq.key_hi = key.hi;
    rq.key_lo = key.lo;
    set_comp_id(rq, comp);

    // Step 1: cache-only probe (no payload sent).
    Header probe = rq;
    probe.flags = flags::kCacheOnly;
    Message reply;
    if (!send_message(s.fd(), probe, nullptr, 0, &err) ||
        recv_message(s.fd(), reply, 5000, 1ull << 32, &err) != RecvResult::Ok) {
      spdlog::error("event=request_failed frame={} reason=\"{}\"", f, err);
      std::printf("summary connection_lost=1\n");
      return 4;
    }
    // Step 2: full request on a miss.
    if (reply.h.status == static_cast<uint16_t>(Status::Miss)) {
      Header full = rq;
      full.timeout_ms = static_cast<uint32_t>(timeout_ms);
      if (!send_message(s.fd(), full, input.data(), input.size(), &err) ||
          recv_message(s.fd(), reply, 60000, 1ull << 32, &err) != RecvResult::Ok) {
        spdlog::error("event=request_failed frame={} reason=\"{}\"", f, err);
        std::printf("summary connection_lost=1\n");
        return 4;
      }
      bytes_moved += input.size();
    }
    const auto st = static_cast<Status>(reply.h.status);
    ++count[to_string(st)];
    const bool has_image = st == Status::Ok || st == Status::CacheHit;
    if (has_image) {
      bytes_moved += reply.payload.size();
      if (!matches(input, reply.payload, transform, tolerance)) {
        ++mismatches;
        spdlog::error("event=pixel_mismatch frame={} status={}", f, to_string(st));
      }
    } else {
      spdlog::debug("event=passthrough frame={} status={} detail=\"{}\"", f, to_string(st),
                    std::string(reply.payload.begin(), reply.payload.end()));
    }
  }

  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::ostringstream sum;
  sum << "frames=" << frames << " mismatches=" << mismatches;
  for (const char* k : {"ok", "cache_hit", "passthrough", "timeout", "skipped", "error", "rejected"})
    sum << " " << k << "=" << count[k];
  sum << " max_rgb_err=" << g_max_rgb_err << " max_alpha_err=" << g_max_alpha_err;
  sum << " elapsed_ms=" << static_cast<long>(ms) << " mb_per_s=" << static_cast<long>(bytes_moved / 1048576.0 / (ms / 1000.0 + 1e-9));
  std::printf("summary %s\n", sum.str().c_str());
  spdlog::info("event=filter_summary {}", sum.str());

  int rc = mismatches ? 5 : 0;
  for (auto& [k, v] : parse_expect(args.str("expect"))) {
    if (count[k] != v) {
      spdlog::error("event=expectation_failed counter={} expected={} actual={}", k, v, count[k]);
      rc = 6;
    }
  }
  return rc;
}
