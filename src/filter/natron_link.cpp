// natron_link - MLT filter module (libmltnatron.so)
//
// WHAT IT DOES
//   For every frame that passes through it, the filter
//     1. fetches the image as RGBA8,
//     2. builds a content key (hash of the pixels + composition file + params + frame),
//     3. asks the daemon whether that key is already rendered (cache-only probe, no pixels sent),
//     4. on a miss sends the frame and waits up to a timeout for the Natron render,
//     5. copies the returned image over the frame, or leaves the frame untouched
//        ("pass-through") if nothing usable came back.
//   Pass-through on any failure is deliberate: a missing daemon or a slow render
//   must never break Kdenlive playback.
//
// PLAYBACK vs EXPORT
//   mode=auto decides per frame, first match wins:
//     1. the frame's consumer has real_time <= 0 (non-realtime consumer)  -> export
//     2. the consumer is a file writer (service name starts with "avformat") -> export
//     3. this process is MLT's command line renderer "melt" and the consumer is
//        not a display/audio device. Kdenlive renders by starting melt (observed:
//        its render consumer reports real_time=1 and no usable service name) -> export
//     otherwise playback. Export waits export_timeout_ms for each frame, playback
//     only playback_timeout_ms. The log line of every frame records which rule fired.
//
// COMPOSITION OF AN EFFECT
//   * "ntp" set (the "Natron project (.ntp)" field in Kdenlive): that file; the
//     composition id is "comp" if set, otherwise the file name without .ntp.
//   * "comp" set, "ntp" empty: <data dir>/comps/<comp>.ntp.
//   * both empty (a freshly added effect): the filter gives the effect its own new
//     composition "comp-xxxxxx" (6 random letters and digits), remembers it in the
//     property nkb_auto_comp and sets "ntp" to <data dir>/comps/comp-xxxxxx.ntp.
//     Both properties are saved with the Kdenlive project. The worker creates that
//     file as a pass-through graph the first time it renders a frame for it. If the
//     ntp field is cleared later, the effect goes back to its own comp-xxxxxx.
//   The file that is hashed into the cache key is always the one named above, so
//   saving the composition in Natron invalidates the cached frames.
//
// OPEN IN NATRON
//   Kdenlive effects have no push buttons, so "open_natron" is a checkbox used as a
//   button: every change of its value (on or off) asks the daemon to open the effect's
//   composition in the Natron GUI (control command "open_natron <path>"). Changes in the
//   first 1.5 s of the filter's life are ignored: that is Kdenlive creating the effect or
//   loading a project, not a click. The melt process (rendering) never opens Natron.
//   The daemon starts Natron because this module runs inside Kdenlive, whose AppImage
//   environment would break another program.
//   Before that, the frame this effect showed last is saved as <name>_preview.tga next
//   to the .ntp; natron/nkb_gui_open.py, run in the Natron GUI, points NKB_Input at it,
//   so Natron shows the clip. The frame is not kept in the melt process (no copy per
//   rendered frame).
//
// THREADS
//   MLT calls get_image from several threads. Each thread keeps its own daemon
//   connection (thread_local), so no locking is needed on the hot path.
//
// PORTABILITY
//   Only the MLT C API and the nkb_base library are used (no spdlog); the module is
//   linked with a static libstdc++ so it loads into snap/AppImage MLT builds.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
#include <framework/mlt.h>
}

#include "nkb/client.h"
#include "nkb/config.h"
#include "nkb/protocol.h"

using namespace nkb;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ------------------------------------------------------------- logging -----
// Same line format as the daemon's spdlog output, appended to the same file.
// One write() per line with O_APPEND keeps lines from different processes intact.
enum Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

struct Logger {
  int fd = -1;
  int threshold = kDebug;
  bool stderr_warnings = true;
  std::string file;
};

Logger& logger() {
  static Logger L;
  static std::once_flag once;
  std::call_once(once, [] {
    Config cfg;
    std::vector<std::string> w;
    cfg.load(config_path(), &w);  // missing file = defaults
    std::string lvl = cfg.get("logging", "level");
    if (const char* e = std::getenv("NKB_LOG_LEVEL")) lvl = e;
    L.threshold = lvl == "error" ? kError : lvl == "warn" ? kWarn : lvl == "info" ? kInfo : kDebug;
    L.file = cfg.get("logging", "log_file");
    if (L.file.empty()) L.file = default_log_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(L.file).parent_path(), ec);
    L.fd = ::open(L.file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  });
  return L;
}

void flog(Level lvl, const char* fmt, ...) {
  Logger& L = logger();
  if (lvl < L.threshold) return;
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  tm tmv;
  gmtime_r(&ts.tv_sec, &tmv);
  static const char* names[] = {"debug", "info", "warning", "error"};
  char line[1200];
  const int n = snprintf(line, sizeof line, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ [%s] [filter] [t=%ld] %s\n",
                         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                         ts.tv_nsec / 1000000, names[lvl], static_cast<long>(syscall(SYS_gettid)), msg);
  if (L.fd >= 0) (void)!write(L.fd, line, static_cast<size_t>(n));
  if (lvl >= kWarn && L.stderr_warnings) (void)!write(2, line, static_cast<size_t>(n));
}

// -------------------------------------------------------- shared setup -----
struct Shared {
  Config cfg;
  std::mutex m;
  std::string token;
  bool token_loaded = false;
};

Shared& shared() {
  static Shared S;
  static std::once_flag once;
  std::call_once(once, [] {
    std::vector<std::string> w;
    S.cfg.load(config_path(), &w);
  });
  return S;
}

bool get_token(std::string* tok, bool reload) {
  Shared& S = shared();
  std::lock_guard<std::mutex> lk(S.m);
  if (!S.token_loaded || reload) {
    std::string err;
    if (!read_token(token_path(), &S.token, &err)) {
      flog(kWarn, "event=token_unavailable reason=\"%s\"", err.c_str());
      return false;
    }
    S.token_loaded = true;
  }
  *tok = S.token;
  return true;
}

// Hash of the composition file, recomputed only when size or mtime change.
Key ntp_key(const std::string& path) {
  static std::mutex m;
  struct Entry { off_t size; timespec mtime; Key key; };
  static std::map<std::string, Entry> cache;
  if (path.empty()) return Key{};
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    // Normal for a new composition until the worker has created it, so this is
    // logged once per path and not for every frame.
    static std::set<std::string> reported;
    const int err = errno;
    bool first;
    {
      std::lock_guard<std::mutex> lk(m);
      first = reported.insert(path).second;
    }
    if (first)
      flog(kInfo, "event=ntp_missing path=\"%s\" reason=\"%s\" note=\"the worker creates a missing composition on its first job\"",
           path.c_str(), strerror(err));
    return KeyBuilder().add("missing-ntp").add(path).finish();
  }
  std::lock_guard<std::mutex> lk(m);
  auto it = cache.find(path);
  if (it != cache.end() && it->second.size == st.st_size && it->second.mtime.tv_sec == st.st_mtim.tv_sec &&
      it->second.mtime.tv_nsec == st.st_mtim.tv_nsec)
    return it->second.key;
  std::vector<char> buf(static_cast<size_t>(st.st_size));
  FILE* f = fopen(path.c_str(), "rb");
  if (!f || (!buf.empty() && fread(buf.data(), 1, buf.size(), f) != buf.size())) {
    if (f) fclose(f);
    flog(kWarn, "event=ntp_unreadable path=\"%s\"", path.c_str());
    return KeyBuilder().add("unreadable-ntp").finish();
  }
  fclose(f);
  Key k = KeyBuilder().add("ntp").add_blob(buf.data(), buf.size()).finish();
  cache[path] = {st.st_size, st.st_mtim, k};
  flog(kInfo, "event=ntp_hashed path=\"%s\" bytes=%ld key=%s", path.c_str(), static_cast<long>(st.st_size),
       k.hex().c_str());
  return k;
}

// -------------------------------------------------------- compositions -----
// Folder of the compositions. Must match the worker: $NKB_COMPS_DIR, otherwise
// <data dir>/comps.
std::string comps_dir() {
  if (const char* e = std::getenv("NKB_COMPS_DIR"); e && *e) return e;
  return home_dir() + "/comps";
}

// File of a composition id, with the same character rules as the worker's
// CompManager.path(): letters, digits, '-', '_' and '.' are kept, others become '_'.
std::string comp_file(const std::string& comp) {
  std::string safe;
  for (unsigned char c : comp) safe += (std::isalnum(c) || c == '-' || c == '_' || c == '.') ? static_cast<char>(c) : '_';
  if (safe.empty()) safe = "default";
  return comps_dir() + "/" + safe + ".ntp";
}

// "comp-" + 6 random lower-case letters and digits (36^6, about 2 billion names),
// skipping names whose file already exists.
std::string new_comp_id() {
  static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  static std::mutex m;
  static std::mt19937_64 rng{std::random_device{}() ^ static_cast<uint64_t>(Clock::now().time_since_epoch().count())};
  std::lock_guard<std::mutex> lk(m);
  for (;;) {
    std::string id = "comp-";
    for (int i = 0; i < 6; ++i) id += chars[rng() % 36];
    struct stat st;
    if (stat(comp_file(id).c_str(), &st) != 0) return id;
  }
}

// ---------------------------------------------------------- connection -----
std::atomic<int64_t> g_no_connect_until_ms{0};  // back-off after a failed connect, shared by all threads

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

struct Conn {
  Socket sock;
  std::string address;
  uint64_t next_id = 1;
};
thread_local Conn tl_conn;

// Returns the thread's connection, connecting if needed. Returns nullptr when
// the daemon is unreachable; after a failure no thread retries for 2 seconds,
// so playback is not slowed by repeated connect attempts.
Conn* connection(const std::string& address_override) {
  Conn& c = tl_conn;
  const std::string address = !address_override.empty() ? address_override
                              : std::getenv("NKB_FILTER_ADDRESS") ? std::getenv("NKB_FILTER_ADDRESS")
                                                                  : shared().cfg.get("daemon", "filter_address");
  if (c.sock.valid() && c.address == address) return &c;
  c.sock.close();
  if (now_ms() < g_no_connect_until_ms.load()) return nullptr;

  Address a;
  std::string err;
  if (!Address::parse(address, &a, &err)) {
    flog(kError, "event=bad_address address=\"%s\" reason=\"%s\"", address.c_str(), err.c_str());
    g_no_connect_until_ms = now_ms() + 2000;
    return nullptr;
  }
  std::string token;
  if (!get_token(&token, false)) {
    g_no_connect_until_ms = now_ms() + 2000;
    return nullptr;
  }
  const auto t0 = Clock::now();
  Socket s = connect_and_handshake(a, Role::Filter, token, 500, &err);
  if (!s.valid() && err.find("bad token") != std::string::npos) {  // daemon may have regenerated its token
    if (get_token(&token, true)) s = connect_and_handshake(a, Role::Filter, token, 500, &err);
  }
  if (!s.valid()) {
    flog(kWarn, "event=daemon_unreachable address=%s reason=\"%s\" retry_in_ms=2000", address.c_str(), err.c_str());
    g_no_connect_until_ms = now_ms() + 2000;
    return nullptr;
  }
  flog(kInfo, "event=daemon_connected address=%s connect_ms=%.1f", address.c_str(), ms_since(t0));
  c.sock = std::move(s);
  c.address = address;
  return &c;
}

void drop_connection(const char* reason) {
  flog(kWarn, "event=connection_dropped reason=\"%s\"", reason);
  tl_conn.sock.close();
}

// ----------------------------------------------------------- request -------
struct Params {
  std::string comp, ntp, address, params;
  std::string hash_path;  // composition file whose content goes into the cache key
  int timeout_ms = 0;
  bool export_mode = false;
};

// Result of asking the daemon. Image => `out` holds the processed RGBA8 frame.
struct Answer {
  bool image = false;
  Status status = Status::Error;
  bool cache_hit = false;
};

Answer ask_daemon(const Params& p, const Key& key, const uint8_t* rgba, uint32_t w, uint32_t h, int64_t frame,
                  std::vector<uint8_t>* out) {
  Answer ans;
  Conn* c = connection(p.address);
  if (!c) {
    ans.status = Status::Error;
    return ans;
  }
  const size_t bytes = static_cast<size_t>(w) * h * 4;

  Header rq = make_header(MsgType::FrameRequest);
  rq.frame_number = frame;
  rq.width = w;
  rq.height = h;
  rq.pixel_format = static_cast<uint16_t>(PixelFormat::RGBA8);
  rq.alpha_mode = static_cast<uint8_t>(AlphaMode::Straight);
  rq.colorspace = static_cast<uint8_t>(Colorspace::Unspecified);
  rq.key_hi = key.hi;
  rq.key_lo = key.lo;
  set_comp_id(rq, p.comp);

  auto exchange = [&](Header h, const void* payload, size_t n, int wait_ms, Message* reply,
                      const std::string& tail = std::string()) {
    h.request_id = c->next_id++;
    h.ntp_len = static_cast<uint32_t>(tail.size());
    std::string err;
    if (!send_message(c->sock.fd(), h, payload, n, tail.data(), tail.size(), &err)) {
      drop_connection(err.c_str());
      return false;
    }
    // The daemon answers within the request timeout; allow some slack for scheduling.
    const RecvResult r = recv_message(c->sock.fd(), *reply, wait_ms + 3000, 1ull << 32, &err);
    if (r != RecvResult::Ok || reply->h.request_id != h.request_id) {
      drop_connection(r == RecvResult::Ok ? "reply does not match request" : err.empty() ? "no reply" : err.c_str());
      return false;
    }
    return true;
  };

  auto accept_image = [&](const Message& m, bool hit) {
    const auto st = static_cast<Status>(m.h.status);
    if ((st == Status::Ok || st == Status::CacheHit) && m.h.width == w && m.h.height == h &&
        m.h.pixel_format == static_cast<uint16_t>(PixelFormat::RGBA8) && m.payload.size() == bytes) {
      *out = m.payload;
      ans.image = true;
      ans.status = st;
      ans.cache_hit = hit;
      return true;
    }
    return false;
  };

  // Step 1: cache-only probe, no pixels on the wire.
  Header probe = rq;
  probe.flags = flags::kCacheOnly;
  Message reply;
  if (!exchange(probe, nullptr, 0, 0, &reply)) return ans;
  if (accept_image(reply, true)) return ans;

  // Step 2: cache miss, send the frame.
  Header full = rq;
  full.timeout_ms = static_cast<uint32_t>(p.timeout_ms);
  // The .ntp path travels after the image, so the worker loads exactly this file.
  const std::string ntp = p.ntp.size() <= kMaxNtpLen ? p.ntp : std::string();
  if (!exchange(full, rgba, bytes, p.timeout_ms, &reply, ntp)) return ans;
  if (accept_image(reply, false)) return ans;
  ans.status = static_cast<Status>(reply.h.status);
  return ans;
}

// ------------------------------------------------------------ MLT glue -----
const char* prop_or(mlt_properties props, const char* name, const char* def) {
  const char* v = mlt_properties_get(props, name);
  return v && *v ? v : def;
}

// The input frame this effect handled last, kept for "Open in Natron" (preview image).
struct LastFrame {
  std::mutex m;
  uint32_t w = 0, h = 0;
  int64_t pos = 0;
  std::vector<uint8_t> rgba;
};

LastFrame* last_frame(mlt_properties props) {
  return static_cast<LastFrame*>(mlt_properties_get_data(props, "_nkb_last_frame", nullptr));
}

// RGBA8 -> 32 bit uncompressed TGA, top-left origin (the format the worker uses).
// Written to a temporary name and renamed, so Natron never reads a half-written file.
bool write_tga(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba, std::string* err) {
  const std::string tmp = path + ".tmp";
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) {
    *err = std::string("cannot write ") + tmp + ": " + strerror(errno);
    return false;
  }
  uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                     static_cast<uint8_t>(w & 0xff), static_cast<uint8_t>(w >> 8),
                     static_cast<uint8_t>(h & 0xff), static_cast<uint8_t>(h >> 8), 32, 0x28};
  std::vector<uint8_t> bgra(rgba);
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  const bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(bgra.data(), 1, bgra.size(), f) == bgra.size();
  if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
    *err = std::string("cannot write ") + path + ": " + strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

// <folder of the .ntp>/<file name without .ntp>_preview.tga (the name natron/nkb_gui_open.py reads).
std::string preview_path(const std::string& ntp) {
  const std::filesystem::path p(ntp);
  return (p.parent_path() / (p.stem().string() + "_preview.tga")).string();
}

// Decides the composition of this effect (see COMPOSITION OF AN EFFECT at the top)
// and gives a new effect its own comp-xxxxxx. Runs under the service lock because
// several MLT threads can render frames of the same effect at the same time.
void resolve_comp(mlt_filter filter, Params* p) {
  mlt_properties props = MLT_FILTER_PROPERTIES(filter);
  mlt_service_lock(MLT_FILTER_SERVICE(filter));
  p->ntp = prop_or(props, "ntp", "");
  p->comp = prop_or(props, "comp", "");
  if (p->ntp.empty() && p->comp.empty()) {
    std::string own = prop_or(props, "nkb_auto_comp", "");
    const bool is_new = own.empty();
    if (is_new) {
      own = new_comp_id();
      mlt_properties_set(props, "nkb_auto_comp", own.c_str());
    }
    p->ntp = comp_file(own);
    mlt_properties_set(props, "ntp", p->ntp.c_str());
    flog(kInfo, "event=comp_assigned comp=%s path=\"%s\" reason=%s", own.c_str(), p->ntp.c_str(),
         is_new ? "new_effect" : "ntp_cleared");
  }
  mlt_service_unlock(MLT_FILTER_SERVICE(filter));
  if (p->comp.empty()) p->comp = std::filesystem::path(p->ntp).stem().string();
  p->hash_path = !p->ntp.empty() ? p->ntp : comp_file(p->comp);
}

int filter_get_image(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height,
                     int writable) {
  (void)writable;
  mlt_filter filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
  mlt_properties props = MLT_FILTER_PROPERTIES(filter);

  // We need RGBA8 and we modify the pixels in place.
  *format = mlt_image_rgba;
  int error = mlt_frame_get_image(frame, image, format, width, height, 1);
  if (error || !*image || *format != mlt_image_rgba || *width <= 0 || *height <= 0) return error;

  const auto t0 = Clock::now();
  Params p;
  resolve_comp(filter, &p);
  p.address = prop_or(props, "address", "");
  p.params = prop_or(props, "params", "");

  // Mode: explicit property, otherwise derived from the consumer and the process.
  const std::string mode = prop_or(props, "mode", "auto");
  mlt_properties consumer = static_cast<mlt_properties>(mlt_properties_get_data(MLT_FRAME_PROPERTIES(frame), "consumer", nullptr));
  std::string consumer_name = "none", real_time_text = "none";
  int real_time = 1;
  if (consumer) {
    const char* svc = mlt_properties_get(consumer, "mlt_service");
    consumer_name = svc && *svc ? svc : "unnamed";
    real_time = mlt_properties_get_int(consumer, "real_time");
    real_time_text = std::to_string(real_time);
  }
  const std::string process = program_invocation_short_name ? program_invocation_short_name : "?";
  const bool display_consumer = consumer_name.rfind("sdl", 0) == 0 || consumer_name == "rtaudio" ||
                                consumer_name.rfind("decklink", 0) == 0 || consumer_name.rfind("qt", 0) == 0;
  const char* export_reason = "none";
  if (mode == "export") export_reason = "property";
  else if (mode == "auto") {
    if (consumer && real_time <= 0) export_reason = "real_time";
    else if (consumer_name.rfind("avformat", 0) == 0) export_reason = "consumer_avformat";
    else if (process.rfind("melt", 0) == 0 && !display_consumer) export_reason = "process_melt";
  }
  p.export_mode = std::strcmp(export_reason, "none") != 0;
  p.timeout_ms = p.export_mode ? mlt_properties_get_int(props, "export_timeout_ms")
                               : mlt_properties_get_int(props, "playback_timeout_ms");

  const int64_t pos = mlt_frame_get_position(frame);
  const uint32_t w = static_cast<uint32_t>(*width), h = static_cast<uint32_t>(*height);

  // Remember the input frame for "Open in Natron" (not while rendering: no use there).
  if (process.rfind("melt", 0) != 0) {
    if (LastFrame* lf = last_frame(props)) {
      std::lock_guard<std::mutex> lk(lf->m);
      lf->w = w;
      lf->h = h;
      lf->pos = pos;
      lf->rgba.assign(*image, *image + static_cast<size_t>(w) * h * 4);
    }
  }

  // Content key: everything the Natron output depends on.
  const auto th = Clock::now();
  const Key key = KeyBuilder()
                      .add("nkb-v1")
                      .add(p.comp)
                      .add_key(ntp_key(p.hash_path))
                      .add(p.params)
                      .add_u64(static_cast<uint64_t>(pos))
                      .add_u64(w)
                      .add_u64(h)
                      .add_blob(*image, static_cast<size_t>(w) * h * 4)
                      .finish();
  const double hash_ms = ms_since(th);

  std::vector<uint8_t> out;
  const Answer ans = ask_daemon(p, key, *image, w, h, pos, &out);
  if (ans.image) std::memcpy(*image, out.data(), out.size());  // replace the frame content

  const char* result = ans.image ? (ans.cache_hit ? "cache_hit" : "rendered") : "passthrough";
  flog(kDebug,
       "event=filter_frame comp=%s frame=%lld size=%ux%u mode=%s export_reason=%s consumer=%s real_time=%s process=%s status=%s result=%s hash_ms=%.1f total_ms=%.1f key=%s",
       p.comp.c_str(), static_cast<long long>(pos), w, h, p.export_mode ? "export" : "playback", export_reason,
       consumer_name.c_str(), real_time_text.c_str(), process.c_str(), to_string(ans.status), result, hash_ms,
       ms_since(t0), key.hex().c_str());
  if (!ans.image && p.export_mode)
    flog(kError, "event=export_frame_unprocessed comp=%s frame=%lld status=%s reason=\"no processed frame within %d ms; the exported frame is NOT processed\"",
         p.comp.c_str(), static_cast<long long>(pos), to_string(ans.status), p.timeout_ms);
  return 0;
}

// --------------------------------------------------------- open in Natron -----
constexpr int64_t kOpenGuardMs = 1500;  // property changes this soon after creation are not clicks

void on_property_changed(mlt_properties props, void* object, mlt_event_data data) {
  const char* name = mlt_event_data_to_string(data);
  if (!name || std::strcmp(name, "open_natron") != 0) return;
  mlt_filter filter = static_cast<mlt_filter>(object);
  const std::string now = prop_or(props, "open_natron", "0");
  const std::string last = prop_or(props, "_nkb_open_last", "0");
  mlt_properties_set(props, "_nkb_open_last", now.c_str());  // '_' properties are not saved
  if (now == last) return;
  const int64_t age_ms = now_ms() - mlt_properties_get_int64(props, "_nkb_created_ms");
  const std::string process = program_invocation_short_name ? program_invocation_short_name : "?";
  if (age_ms < kOpenGuardMs) {
    flog(kDebug, "event=open_natron_ignored value=%s age_ms=%lld reason=\"set while the effect was created or loaded\"",
         now.c_str(), static_cast<long long>(age_ms));
    return;
  }
  if (process.rfind("melt", 0) == 0) {
    flog(kDebug, "event=open_natron_ignored value=%s reason=\"melt process (rendering)\"", now.c_str());
    return;
  }
  Params p;
  resolve_comp(filter, &p);
  const std::string path = p.hash_path;
  const std::string override_address = prop_or(props, "address", "");
  auto frame_copy = std::make_shared<LastFrame>();
  if (LastFrame* lf = last_frame(props)) {
    std::lock_guard<std::mutex> lk(lf->m);
    frame_copy->w = lf->w;
    frame_copy->h = lf->h;
    frame_copy->pos = lf->pos;
    frame_copy->rgba = lf->rgba;
  }
  flog(kInfo, "event=open_natron_clicked comp=%s path=\"%s\"", p.comp.c_str(), path.c_str());
  // Write the preview and talk to the daemon on a separate thread: this runs on Kdenlive's GUI thread.
  std::thread([path, override_address, frame_copy] {
    if (frame_copy->rgba.empty()) {
      flog(kInfo, "event=preview_skipped path=\"%s\" reason=\"the effect has not shown a frame yet\"", path.c_str());
    } else {
      std::string perr;
      const std::string pv = preview_path(path);
      if (write_tga(pv, frame_copy->w, frame_copy->h, frame_copy->rgba, &perr))
        flog(kInfo, "event=preview_written path=\"%s\" frame=%lld size=%ux%u", pv.c_str(),
             static_cast<long long>(frame_copy->pos), frame_copy->w, frame_copy->h);
      else
        flog(kWarn, "event=preview_failed reason=\"%s\"", perr.c_str());
    }
    const std::string address = !override_address.empty() ? override_address
                                : std::getenv("NKB_FILTER_ADDRESS") ? std::getenv("NKB_FILTER_ADDRESS")
                                                                    : shared().cfg.get("daemon", "filter_address");
    Address a;
    std::string token, reply, err;
    if (!Address::parse(address, &a, &err) || !get_token(&token, false) ||
        !control_request(a, token, "open_natron " + path, &reply, &err)) {
      flog(kWarn, "event=open_natron_failed path=\"%s\" reason=\"%s\" hint=\"is natron-kdenlive-daemon running?\"",
           path.c_str(), err.empty() ? reply.c_str() : err.c_str());
      return;
    }
    flog(kInfo, "event=open_natron_sent path=\"%s\" reply=\"%s\"", path.c_str(), reply.c_str());
  }).detach();
}

mlt_frame filter_process(mlt_filter filter, mlt_frame frame) {
  mlt_frame_push_service(frame, filter);
  mlt_frame_push_get_image(frame, filter_get_image);
  return frame;
}

extern "C" mlt_filter filter_natron_link_init(mlt_profile, mlt_service_type, const char*, void*) {
  mlt_filter filter = mlt_filter_new();
  if (!filter) return nullptr;
  filter->process = filter_process;
  mlt_properties props = MLT_FILTER_PROPERTIES(filter);
  mlt_properties_set(props, "ntp", "");                      // path of the Natron project (.ntp)
  mlt_properties_set(props, "comp", "");                     // composition id; default = file name of ntp
  // nkb_auto_comp is not set here: an effect gets its own comp-xxxxxx when it renders
  // its first frame with ntp and comp empty (see resolve_comp).
  mlt_properties_set(props, "mode", "auto");                 // auto | playback | export
  mlt_properties_set_int(props, "playback_timeout_ms", 250); // wait for a render during playback
  mlt_properties_set_int(props, "export_timeout_ms", 600000);// wait for a render during export
  mlt_properties_set(props, "open_natron", "0");             // checkbox used as a button, see OPEN IN NATRON
  mlt_properties_set(props, "_nkb_open_last", "0");
  mlt_properties_set_int64(props, "_nkb_created_ms", now_ms());
  mlt_properties_set_data(props, "_nkb_last_frame", new LastFrame, 0,
                          [](void* p) { delete static_cast<LastFrame*>(p); }, nullptr);
  mlt_events_listen(props, filter, "property-changed", reinterpret_cast<mlt_listener>(on_property_changed));
  flog(kInfo, "event=filter_created");
  return filter;
}

}  // namespace

// ---------------------------------------------------------- metadata ------
// Kdenlive asks MLT for the metadata of every effect it finds an XML file for
// (repository->metadata(filter, id)). Without it Kdenlive logs "Invalid metadata
// for natron_link" and refuses the effect. MLT's own modules read a .yml file from
// MLT_DATA; this module builds the same structure in code so that no extra data
// file has to be installed next to the module.
void add_param(mlt_properties list, int index, const char* id, const char* title, const char* type,
               const char* def, const char* description) {
  mlt_properties p = mlt_properties_new();
  mlt_properties_set(p, "identifier", id);
  mlt_properties_set(p, "title", title);
  mlt_properties_set(p, "type", type);
  if (def) mlt_properties_set(p, "default", def);
  mlt_properties_set(p, "description", description);
  char name[16];
  snprintf(name, sizeof name, "%d", index);
  mlt_properties_set_data(list, name, p, 0, reinterpret_cast<mlt_destructor>(mlt_properties_close), nullptr);
}

extern "C" mlt_properties natron_link_metadata(mlt_service_type, const char*, void*) {
  mlt_properties m = mlt_properties_new();
  if (!m) return nullptr;
  mlt_properties_set(m, "schema_version", "7.2");
  mlt_properties_set(m, "type", "filter");
  mlt_properties_set(m, "identifier", "natron_link");
  mlt_properties_set(m, "title", "Natron Link");
  mlt_properties_set(m, "version", "1");
  mlt_properties_set(m, "creator", "natron-kdenlive-link");
  mlt_properties_set(m, "license", "GPL-3.0-or-later");
  mlt_properties_set(m, "language", "en");
  mlt_properties_set(m, "description",
                     "Sends the frames to Natron through the natron-kdenlive-daemon and shows the result. "
                     "Frames pass through unchanged when Natron is unavailable or too slow.");

  mlt_properties tags = mlt_properties_new();
  mlt_properties_set(tags, "0", "Video");
  mlt_properties_set_data(m, "tags", tags, 0, reinterpret_cast<mlt_destructor>(mlt_properties_close), nullptr);

  mlt_properties formats = mlt_properties_new();
  mlt_properties_set(formats, "0", "rgba");
  mlt_properties_set_data(m, "image_formats", formats, 0, reinterpret_cast<mlt_destructor>(mlt_properties_close), nullptr);

  mlt_properties params = mlt_properties_new();
  add_param(params, 0, "ntp", "Natron project", "string", "", "Path of the Natron project file (.ntp)");
  add_param(params, 1, "comp", "Composition id", "string", "",
            "Composition id; defaults to the file name of ntp. With ntp and comp empty the effect gets its own comp-xxxxxx");
  add_param(params, 2, "mode", "Mode", "string", "auto", "auto, playback or export");
  add_param(params, 3, "playback_timeout_ms", "Playback wait (ms)", "integer", "250",
            "How long to wait for a render during playback");
  add_param(params, 4, "export_timeout_ms", "Export wait (ms)", "integer", "600000",
            "How long to wait for a render during export");
  add_param(params, 5, "address", "Daemon address", "string", "", "Overrides filter_address from config.ini");
  add_param(params, 6, "params", "Extra key data", "string", "", "Text hashed into the cache key");
  add_param(params, 7, "open_natron", "Open in Natron", "boolean", "0",
            "Every change of this value opens the composition in the Natron GUI (through the daemon)");
  mlt_properties_set_data(m, "parameters", params, 0, reinterpret_cast<mlt_destructor>(mlt_properties_close), nullptr);
  return m;
}

// Entry point looked up by MLT when it loads libmltnatron.so.
extern "C" {
MLT_REPOSITORY {
  MLT_REGISTER(mlt_service_filter_type, "natron_link", filter_natron_link_init);
  MLT_REGISTER_METADATA(mlt_service_filter_type, "natron_link", natron_link_metadata, nullptr);
}
}
