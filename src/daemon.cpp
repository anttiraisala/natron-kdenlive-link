// natron-kdenlive-daemon
//
// ROLE
//   Sits between the MLT filter (inside Kdenlive) and the Natron worker.
//   Playback is PULL based: the filter asks for frame N (identified by a
//   content key). The daemon answers from the output cache, or queues a render
//   job for a worker and waits up to the request timeout.
//
// THREADS
//   main        stats logging loop, waits for SIGINT/SIGTERM
//   acceptor    polls both listeners, spawns one thread per connection
//   connection  one per client; filters/tools run serve_filter(), workers
//               run serve_worker() (pop job -> send -> wait for result)
//
// SHARED STATE
//   cache_      output cache (internally locked)
//   queue_ etc. input queue + in-flight map, guarded by qm_
//   Job         guarded by its own mutex; filters wait on it, workers complete it
#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <poll.h>

#include "nkb/cache.h"
#include "nkb/config.h"
#include "nkb/log.h"
#include "nkb/net.h"
#include "nkb/protocol.h"

using namespace nkb;
using Clock = std::chrono::steady_clock;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

enum class Behavior { Pause, BufferSkip, ShowCached };

// One render request. Several filters asking for the same key share one Job.
struct Job {
  Key key;
  Header req{};                                   // geometry etc. of the input
  std::shared_ptr<std::vector<uint8_t>> input;    // raw input frame
  uint64_t id = 0;
  Clock::time_point enqueued = Clock::now();

  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  Status status = Status::Error;
  std::shared_ptr<const Frame> result;
  std::string detail;  // reason for non-Ok statuses
};

class Daemon {
 public:
  explicit Daemon(const Config& cfg, std::string token) : cfg_(cfg), token_(std::move(token)), cache_(0) {}

  bool start(std::string* err);
  void run();

 private:
  // ---- connection handling
  void accept_loop();
  void connection_main(Socket s, bool worker_listener, uint64_t conn);
  bool handshake(Socket& s, bool worker_listener, Role* role, uint64_t conn);
  void serve_filter(Socket& s, Role role, uint64_t conn);
  void serve_worker(Socket& s, uint64_t conn);
  bool handle_frame_request(Socket& s, Message& m, uint64_t conn);
  bool handle_control(Socket& s, const Message& m, uint64_t conn);
  bool reply_status(Socket& s, const Header& req, Status st, const std::string& text);
  bool reply_frame(Socket& s, const Header& req, Status st, const Frame& f);

  // ---- queue
  struct Submit {
    std::shared_ptr<Job> job;   // null if rejected
    Status reject = Status::Ok;
    std::string detail;
    bool deduped = false;
  };
  Submit submit(const Key& key, const Header& req, std::shared_ptr<std::vector<uint8_t>> input, uint32_t timeout_ms);
  std::shared_ptr<Job> pop_job(int wait_ms);
  void finish(const std::shared_ptr<Job>& j, Status st, std::shared_ptr<const Frame> f, const std::string& detail);
  bool wait_job(const std::shared_ptr<Job>& j, uint32_t timeout_ms);

  // ---- stats
  std::vector<std::pair<std::string, uint64_t>> stats_pairs();
  std::string stats_text();
  std::string stats_line();

  Config cfg_;
  std::string token_;
  FrameCache cache_;
  Address filter_addr_, worker_addr_;
  Socket filter_listener_, worker_listener_;
  Behavior behavior_ = Behavior::BufferSkip;
  uint64_t max_frame_bytes_ = 0;
  uint64_t queue_limit_bytes_ = 0;
  uint32_t default_timeout_ms_ = 1000;
  uint32_t no_worker_wait_ms_ = 0;
  int worker_timeout_ms_ = 10000;

  std::mutex qm_;
  std::condition_variable cv_jobs_;   // wakes workers
  std::condition_variable cv_space_;  // wakes filters waiting in pause mode
  std::deque<std::shared_ptr<Job>> queue_;
  std::unordered_map<Key, std::shared_ptr<Job>, KeyHash> inflight_;  // queued + running
  uint64_t queue_bytes_ = 0;
  uint64_t next_job_id_ = 1;

  std::atomic<int> active_{0};
  std::atomic<uint64_t> next_conn_{1};
  std::atomic<uint64_t> n_submitted_{0}, n_completed_{0}, n_skipped_{0}, n_rejected_{0},
      n_waiter_timeouts_{0}, n_job_errors_{0}, n_dedup_{0};
  std::atomic<int> n_filters_{0}, n_workers_{0};
  std::atomic<uint64_t> n_worker_connections_{0};  // ever connected; lets tests tell a new worker from a dying one
};

// ------------------------------------------------------------- startup -----
bool Daemon::start(std::string* err) {
  std::string e;
  if (!Address::parse(cfg_.get("daemon", "filter_address"), &filter_addr_, &e) ||
      !Address::parse(cfg_.get("daemon", "worker_address"), &worker_addr_, &e)) {
    *err = e;
    return false;
  }
  if (filter_addr_.str() == worker_addr_.str()) {
    *err = "filter_address and worker_address must differ";
    return false;
  }

  // Cache budget = target clamped into [min, max].
  int64_t target = cfg_.get_int("daemon", "cache_memory_mb");
  const int64_t lo = cfg_.get_int("daemon", "cache_memory_min_mb");
  const int64_t hi = cfg_.get_int("daemon", "cache_memory_max_mb");
  if (lo > hi) {
    *err = "cache_memory_min_mb is larger than cache_memory_max_mb";
    return false;
  }
  const int64_t clamped = std::min(std::max(target, lo), hi);
  if (clamped != target)
    spdlog::warn("event=cache_size_clamped requested_mb={} used_mb={} min_mb={} max_mb={}", target, clamped, lo, hi);
  target = clamped;
  cache_.set_target_bytes(static_cast<uint64_t>(target) << 20);

  queue_limit_bytes_ = static_cast<uint64_t>(cfg_.get_int("daemon", "input_queue_memory_mb")) << 20;
  max_frame_bytes_ = static_cast<uint64_t>(cfg_.get_int("daemon", "max_frame_mb")) << 20;
  default_timeout_ms_ = static_cast<uint32_t>(cfg_.get_int("daemon", "default_request_timeout_ms"));
  no_worker_wait_ms_ = static_cast<uint32_t>(cfg_.get_int("daemon", "no_worker_wait_ms"));
  worker_timeout_ms_ = static_cast<int>(cfg_.get_int("daemon", "natron_timeout_seconds")) * 1000;
  const std::string b = cfg_.get("daemon", "buffer_behavior");
  behavior_ = b == "pause" ? Behavior::Pause : b == "show_cached" ? Behavior::ShowCached : Behavior::BufferSkip;

  filter_listener_ = listen_on(filter_addr_, &e);
  if (!filter_listener_.valid()) { *err = e; return false; }
  worker_listener_ = listen_on(worker_addr_, &e);
  if (!worker_listener_.valid()) { *err = e; return false; }

  spdlog::info("event=daemon_start data_dir={} cache_mb={} cache_min_mb={} cache_max_mb={} input_queue_mb={} "
               "buffer_behavior={} worker_timeout_s={} default_request_timeout_ms={} max_frame_mb={}",
               home_dir(), target, lo, hi, queue_limit_bytes_ >> 20, b, worker_timeout_ms_ / 1000,
               default_timeout_ms_, max_frame_bytes_ >> 20);
  spdlog::info("event=listening role=filters address={}", filter_addr_.str());
  spdlog::info("event=listening role=workers address={}", worker_addr_.str());
  return true;
}

void Daemon::run() {
  std::thread acceptor([this] { accept_loop(); });
  const int interval = static_cast<int>(cfg_.get_int("daemon", "stats_log_interval_seconds"));
  auto last = Clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (interval > 0 && ms_since(last) >= interval * 1000.0) {
      spdlog::info("event=stats {}", stats_line());
      last = Clock::now();
    }
  }
  spdlog::info("event=daemon_stopping active_connections={}", active_.load());
  cv_jobs_.notify_all();
  cv_space_.notify_all();
  acceptor.join();
  // Connection threads poll g_stop at least every 500 ms.
  for (int i = 0; i < 100 && active_ > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (active_ > 0) spdlog::warn("event=daemon_stop_forced still_active={}", active_.load());
  spdlog::info("event=daemon_stopped {}", stats_line());
}

void Daemon::accept_loop() {
  while (!g_stop) {
    pollfd p[2] = {{filter_listener_.fd(), POLLIN, 0}, {worker_listener_.fd(), POLLIN, 0}};
    if (poll(p, 2, 250) <= 0) continue;
    for (int i = 0; i < 2; ++i) {
      if (!(p[i].revents & POLLIN)) continue;
      Socket s = accept_on(p[i].fd, 0);
      if (!s.valid()) continue;
      const uint64_t conn = next_conn_++;
      const bool is_worker = i == 1;
      ++active_;
      std::thread([this, s = std::move(s), is_worker, conn]() mutable {
        connection_main(std::move(s), is_worker, conn);
        --active_;
      }).detach();
    }
  }
}

// -------------------------------------------------------- connections ------
bool Daemon::handshake(Socket& s, bool worker_listener, Role* role, uint64_t conn) {
  Message m;
  std::string err;
  const RecvResult r = recv_message(s.fd(), m, 5000, 4096, &err);
  auto reject = [&](const std::string& why) {
    spdlog::warn("event=handshake_failed conn={} reason=\"{}\"", conn, why);
    Header h = make_header(MsgType::HelloAck);
    h.status = static_cast<uint16_t>(Status::Error);
    send_message(s.fd(), h, why.data(), why.size(), nullptr);
    return false;
  };
  if (r != RecvResult::Ok) return reject(err.empty() ? "no Hello received" : err);
  if (m.h.type != static_cast<uint16_t>(MsgType::Hello)) return reject("first message must be Hello");
  // Token compare without early exit.
  const std::string given(m.payload.begin(), m.payload.end());
  unsigned diff = given.size() ^ token_.size();
  for (size_t i = 0; i < given.size() && i < token_.size(); ++i) diff |= given[i] ^ token_[i];
  if (diff != 0) return reject("bad token");
  const auto want = static_cast<Role>(m.h.flags);
  const bool ok = worker_listener ? want == Role::Worker : (want == Role::Filter || want == Role::Tool);
  if (!ok) return reject(std::string("role ") + to_string(want) + " not allowed on this port");
  *role = want;
  const std::string info = "natron-kdenlive-daemon proto=" + std::to_string(kProtocolVersion);
  Header h = make_header(MsgType::HelloAck);
  h.status = static_cast<uint16_t>(Status::Ok);
  return send_message(s.fd(), h, info.data(), info.size(), nullptr);
}

void Daemon::connection_main(Socket s, bool worker_listener, uint64_t conn) {
  Role role{};
  spdlog::debug("event=conn_accepted conn={} port_role={}", conn, worker_listener ? "workers" : "filters");
  if (!handshake(s, worker_listener, &role, conn)) return;
  spdlog::info("event=handshake_ok conn={} role={}", conn, to_string(role));
  if (role == Role::Worker) {
    ++n_workers_;
    ++n_worker_connections_;
    serve_worker(s, conn);
    --n_workers_;
  } else {
    if (role == Role::Filter) ++n_filters_;
    serve_filter(s, role, conn);
    if (role == Role::Filter) --n_filters_;
  }
  spdlog::info("event=conn_closed conn={} role={}", conn, to_string(role));
}

bool Daemon::reply_status(Socket& s, const Header& req, Status st, const std::string& text) {
  Header h = make_header(MsgType::FrameResult);
  h.request_id = req.request_id;
  h.frame_number = req.frame_number;
  h.key_hi = req.key_hi;
  h.key_lo = req.key_lo;
  h.status = static_cast<uint16_t>(st);
  std::memcpy(h.comp_id, req.comp_id, kCompIdLen);
  std::string err;
  if (!send_message(s.fd(), h, text.data(), text.size(), &err)) {
    spdlog::warn("event=reply_failed request_id={} reason=\"{}\"", req.request_id, err);
    return false;
  }
  return true;
}

bool Daemon::reply_frame(Socket& s, const Header& req, Status st, const Frame& f) {
  Header h = make_header(MsgType::FrameResult);
  h.request_id = req.request_id;
  h.frame_number = req.frame_number;
  h.key_hi = req.key_hi;
  h.key_lo = req.key_lo;
  h.status = static_cast<uint16_t>(st);
  h.width = f.meta.width;
  h.height = f.meta.height;
  h.pixel_format = static_cast<uint16_t>(f.meta.pixel_format);
  h.alpha_mode = static_cast<uint8_t>(f.meta.alpha);
  h.colorspace = static_cast<uint8_t>(f.meta.colorspace);
  std::memcpy(h.comp_id, req.comp_id, kCompIdLen);
  std::string err;
  if (!send_message(s.fd(), h, f.data.data(), f.data.size(), &err)) {
    spdlog::warn("event=reply_failed request_id={} reason=\"{}\"", req.request_id, err);
    return false;
  }
  return true;
}

// ------------------------------------------------------------ filters ------
void Daemon::serve_filter(Socket& s, Role role, uint64_t conn) {
  while (!g_stop) {
    Message m;
    std::string err;
    const RecvResult r = recv_message(s.fd(), m, 500, max_frame_bytes_, &err);
    if (r == RecvResult::Timeout) continue;
    if (r == RecvResult::Closed) return;
    if (r != RecvResult::Ok) {
      spdlog::warn("event=recv_failed conn={} reason=\"{}\"", conn, err);
      return;
    }
    bool ok = true;
    switch (static_cast<MsgType>(m.h.type)) {
      case MsgType::FrameRequest:
        ok = role == Role::Filter ? handle_frame_request(s, m, conn)
                                  : reply_status(s, m.h, Status::Error, "tools may not request frames");
        break;
      case MsgType::Control:
        ok = handle_control(s, m, conn);
        break;
      default:
        spdlog::warn("event=unexpected_message conn={} type={}", conn, to_string(static_cast<MsgType>(m.h.type)));
        ok = false;
    }
    if (!ok) return;
  }
}

bool Daemon::handle_frame_request(Socket& s, Message& m, uint64_t conn) {
  const auto t0 = Clock::now();
  const Header& h = m.h;
  const Key key{h.key_hi, h.key_lo};
  const std::string comp = get_comp_id(h);
  const bool cache_only = (h.flags & flags::kCacheOnly) != 0;
  uint32_t timeout_ms = h.timeout_ms == kTimeoutDefault ? default_timeout_ms_ : h.timeout_ms;
  // Nobody can render right now: do not make playback wait for a result that cannot arrive.
  // The job is still queued and is rendered as soon as a worker connects.
  if (n_workers_ == 0 && timeout_ms > no_worker_wait_ms_) {
    spdlog::debug("event=no_worker request_id={} requested_timeout_ms={} used_timeout_ms={}", h.request_id, timeout_ms,
                  no_worker_wait_ms_);
    timeout_ms = no_worker_wait_ms_;
  }

  uint64_t expected = 0;
  if (!expected_payload_bytes(h, max_frame_bytes_, &expected)) {
    spdlog::warn("event=bad_request conn={} request_id={} reason=\"invalid geometry or format\" w={} h={} fmt={}",
                 conn, h.request_id, h.width, h.height, h.pixel_format);
    return reply_status(s, h, Status::Error, "invalid geometry or pixel format");
  }

  if (auto f = cache_.get(key)) {
    spdlog::debug("event=cache_hit conn={} request_id={} comp={} frame={} key={} bytes={}", conn, h.request_id,
                  comp, h.frame_number, key.hex(), f->data.size());
    return reply_frame(s, h, Status::CacheHit, *f);
  }
  if (cache_only) {
    spdlog::debug("event=cache_miss conn={} request_id={} comp={} frame={} key={} mode=cache_only", conn,
                  h.request_id, comp, h.frame_number, key.hex());
    return reply_status(s, h, Status::Miss, "");
  }
  if (m.payload.size() != expected) {
    spdlog::warn("event=bad_request conn={} request_id={} reason=\"payload size mismatch\" got={} expected={}", conn,
                 h.request_id, m.payload.size(), expected);
    return reply_status(s, h, Status::Error, "payload size does not match geometry");
  }

  spdlog::debug("event=cache_miss conn={} request_id={} comp={} frame={} key={} bytes={} timeout_ms={}", conn,
                h.request_id, comp, h.frame_number, key.hex(), m.payload.size(), timeout_ms);
  auto input = std::make_shared<std::vector<uint8_t>>(std::move(m.payload));
  Submit sub = submit(key, h, input, timeout_ms);
  if (!sub.job) {
    spdlog::info("event=request_rejected conn={} request_id={} key={} status={} reason=\"{}\"", conn, h.request_id,
                 key.hex(), to_string(sub.reject), sub.detail);
    return reply_status(s, h, sub.reject, sub.detail);
  }

  const bool finished = wait_job(sub.job, timeout_ms);
  if (!finished) {
    ++n_waiter_timeouts_;
    // timeout 0 is the playback mode: "queue it, I will pull the result later".
    const Status st = timeout_ms == 0 ? Status::Passthrough : Status::Timeout;
    spdlog::debug("event=request_not_ready conn={} request_id={} key={} status={} waited_ms={:.1f} deduped={}", conn,
                  h.request_id, key.hex(), to_string(st), ms_since(t0), sub.deduped);
    return reply_status(s, h, st, "render queued, result not ready");
  }
  std::unique_lock<std::mutex> lk(sub.job->m);
  const Status st = sub.job->status;
  auto result = sub.job->result;
  const std::string detail = sub.job->detail;
  lk.unlock();
  if (st == Status::Ok && result) {
    spdlog::debug("event=request_done conn={} request_id={} key={} status=ok total_ms={:.1f} bytes={}", conn,
                  h.request_id, key.hex(), ms_since(t0), result->data.size());
    return reply_frame(s, h, Status::Ok, *result);
  }
  spdlog::info("event=request_failed conn={} request_id={} key={} status={} reason=\"{}\"", conn, h.request_id,
               key.hex(), to_string(st), detail);
  return reply_status(s, h, st, detail);
}

bool Daemon::handle_control(Socket& s, const Message& m, uint64_t conn) {
  const std::string cmd(m.payload.begin(), m.payload.end());
  std::string out;
  if (cmd == "ping") {
    out = "pong";
  } else if (cmd == "stats") {
    out = stats_text();
  } else if (cmd == "clear") {
    const auto r = cache_.clear();
    out = "cleared_entries=" + std::to_string(r.entries) + "\ncleared_bytes=" + std::to_string(r.bytes) + "\n";
    spdlog::info("event=cache_cleared conn={} entries={} bytes={}", conn, r.entries, r.bytes);
  } else {
    spdlog::warn("event=unknown_control conn={} cmd=\"{}\"", conn, cmd);
    Header h = make_header(MsgType::ControlReply);
    h.status = static_cast<uint16_t>(Status::Error);
    const std::string msg = "unknown command: " + cmd;
    return send_message(s.fd(), h, msg.data(), msg.size(), nullptr);
  }
  spdlog::debug("event=control conn={} cmd={}", conn, cmd);
  Header h = make_header(MsgType::ControlReply);
  h.status = static_cast<uint16_t>(Status::Ok);
  return send_message(s.fd(), h, out.data(), out.size(), nullptr);
}

// -------------------------------------------------------------- queue ------
Daemon::Submit Daemon::submit(const Key& key, const Header& req, std::shared_ptr<std::vector<uint8_t>> input,
                              uint32_t timeout_ms) {
  const uint64_t bytes = input->size();
  std::unique_lock<std::mutex> lk(qm_);

  // Same key already queued or rendering: share that job instead of a second render.
  if (auto it = inflight_.find(key); it != inflight_.end()) {
    ++n_dedup_;
    spdlog::debug("event=job_deduplicated key={} job={}", key.hex(), it->second->id);
    return {it->second, Status::Ok, "", true};
  }

  // A frame larger than the whole queue limit is still accepted when the queue
  // is empty, otherwise it could never be processed.
  auto over = [&] { return !queue_.empty() && queue_bytes_ + bytes > queue_limit_bytes_; };
  if (over()) {
    switch (behavior_) {
      case Behavior::ShowCached:
        ++n_rejected_;
        return {nullptr, Status::Rejected, "input queue full (show_cached)", false};
      case Behavior::BufferSkip:
        while (over()) {
          auto old = queue_.front();
          queue_.pop_front();
          queue_bytes_ -= old->input->size();
          inflight_.erase(old->key);
          ++n_skipped_;
          spdlog::info("event=job_skipped job={} key={} frame={} queued_ms={:.1f} reason=\"input queue full (buffer_skip)\"",
                       old->id, old->key.hex(), old->req.frame_number, ms_since(old->enqueued));
          lk.unlock();
          finish(old, Status::Skipped, nullptr, "dropped from input queue (buffer_skip)");
          lk.lock();
        }
        break;
      case Behavior::Pause: {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (over() && !g_stop) {
          if (cv_space_.wait_until(lk, deadline) == std::cv_status::timeout) break;
        }
        if (over()) {
          ++n_rejected_;
          return {nullptr, Status::Timeout, "input queue full and no space freed in time (pause)", false};
        }
        // The key may have been queued by someone else while we waited.
        if (auto it = inflight_.find(key); it != inflight_.end()) {
          ++n_dedup_;
          return {it->second, Status::Ok, "", true};
        }
        break;
      }
    }
  }

  // The lock was released while skipping; another request may have queued this key meanwhile.
  if (auto it = inflight_.find(key); it != inflight_.end()) {
    ++n_dedup_;
    return {it->second, Status::Ok, "", true};
  }
  auto job = std::make_shared<Job>();
  job->key = key;
  job->req = req;
  job->input = std::move(input);
  job->id = next_job_id_++;
  queue_.push_back(job);
  queue_bytes_ += bytes;
  inflight_[key] = job;
  ++n_submitted_;
  spdlog::debug("event=job_queued job={} key={} comp={} frame={} queue_jobs={} queue_mb={:.1f} workers={}", job->id,
                key.hex(), get_comp_id(req), req.frame_number, queue_.size(), queue_bytes_ / 1048576.0,
                n_workers_.load());
  cv_jobs_.notify_one();
  return {job, Status::Ok, "", false};
}

std::shared_ptr<Job> Daemon::pop_job(int wait_ms) {
  std::unique_lock<std::mutex> lk(qm_);
  cv_jobs_.wait_for(lk, std::chrono::milliseconds(wait_ms), [&] { return !queue_.empty() || g_stop; });
  if (queue_.empty()) return nullptr;
  auto j = queue_.front();
  queue_.pop_front();
  queue_bytes_ -= j->input->size();
  cv_space_.notify_all();
  return j;  // stays in inflight_ until finish()
}

void Daemon::finish(const std::shared_ptr<Job>& j, Status st, std::shared_ptr<const Frame> f,
                    const std::string& detail) {
  {
    std::lock_guard<std::mutex> lk(qm_);
    if (auto it = inflight_.find(j->key); it != inflight_.end() && it->second == j) inflight_.erase(it);
  }
  {
    std::lock_guard<std::mutex> lk(j->m);
    j->done = true;
    j->status = st;
    j->result = std::move(f);
    j->detail = detail;
  }
  j->cv.notify_all();
}

bool Daemon::wait_job(const std::shared_ptr<Job>& j, uint32_t timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock<std::mutex> lk(j->m);
  while (!j->done && !g_stop) {
    // Wake in slices so shutdown is never delayed by a long wait.
    const auto slice = std::min(deadline, Clock::now() + std::chrono::milliseconds(200));
    if (Clock::now() >= deadline) break;
    j->cv.wait_until(lk, slice);
  }
  return j->done;
}

// ------------------------------------------------------------ workers ------
void Daemon::serve_worker(Socket& s, uint64_t conn) {
  while (!g_stop) {
    auto job = pop_job(200);
    if (!job) {
      // Idle: a worker never speaks unprompted, so readable means closed/garbage.
      pollfd p{s.fd(), POLLIN, 0};
      if (poll(&p, 1, 0) > 0) {
        spdlog::info("event=worker_disconnected conn={} while=idle", conn);
        return;
      }
      continue;
    }
    const double queued_ms = ms_since(job->enqueued);
    Header h = job->req;
    h.type = static_cast<uint16_t>(MsgType::Job);
    h.request_id = job->id;
    h.flags = 0;
    spdlog::debug("event=job_sent job={} conn={} key={} frame={} queued_ms={:.1f} bytes={}", job->id, conn,
                  job->key.hex(), job->req.frame_number, queued_ms, job->input->size());
    const auto t0 = Clock::now();
    std::string err;
    if (!send_message(s.fd(), h, job->input->data(), job->input->size(), &err)) {
      ++n_job_errors_;
      spdlog::error("event=job_send_failed job={} conn={} reason=\"{}\"", job->id, conn, err);
      finish(job, Status::Error, nullptr, "could not send job to worker: " + err);
      return;
    }
    Message res;
    const RecvResult r = recv_message(s.fd(), res, worker_timeout_ms_, max_frame_bytes_, &err);
    if (r != RecvResult::Ok) {
      ++n_job_errors_;
      const char* why = r == RecvResult::Timeout ? "worker timeout" : r == RecvResult::Closed ? "worker closed connection" : "worker protocol error";
      spdlog::error("event=job_failed job={} conn={} key={} frame={} reason=\"{}\" detail=\"{}\" waited_ms={:.1f} "
                    "worker_timeout_s={}",
                    job->id, conn, job->key.hex(), job->req.frame_number, why, err, ms_since(t0),
                    worker_timeout_ms_ / 1000);
      finish(job, r == RecvResult::Timeout ? Status::Timeout : Status::Error, nullptr, why);
      return;  // drop the connection; a restarted worker reconnects
    }
    uint64_t expected = 0;
    const bool geometry_ok = expected_payload_bytes(res.h, max_frame_bytes_, &expected);
    if (res.h.type != static_cast<uint16_t>(MsgType::JobResult) || res.h.request_id != job->id) {
      ++n_job_errors_;
      spdlog::error("event=job_failed job={} conn={} reason=\"unexpected reply\" type={} reply_id={}", job->id, conn,
                    res.h.type, res.h.request_id);
      finish(job, Status::Error, nullptr, "unexpected reply from worker");
      return;
    }
    if (res.h.status != static_cast<uint16_t>(Status::Ok)) {
      ++n_job_errors_;
      const std::string text(res.payload.begin(), res.payload.end());
      spdlog::error("event=job_failed job={} conn={} key={} frame={} reason=\"worker reported error\" detail=\"{}\"",
                    job->id, conn, job->key.hex(), job->req.frame_number, text);
      finish(job, Status::Error, nullptr, "worker error: " + text);
      continue;  // worker is alive, keep serving
    }
    if (!geometry_ok || res.payload.size() != expected) {
      ++n_job_errors_;
      spdlog::error("event=job_failed job={} conn={} reason=\"result geometry/payload mismatch\" payload={} expected={}",
                    job->id, conn, res.payload.size(), expected);
      finish(job, Status::Error, nullptr, "worker returned an invalid frame");
      continue;
    }
    auto frame = std::make_shared<Frame>();
    frame->meta = {res.h.width, res.h.height, static_cast<PixelFormat>(res.h.pixel_format),
                   static_cast<AlphaMode>(res.h.alpha_mode), static_cast<Colorspace>(res.h.colorspace),
                   job->req.frame_number};
    frame->data = std::move(res.payload);
    const size_t evicted = cache_.put(job->key, frame);
    ++n_completed_;
    const auto cs = cache_.stats();
    spdlog::debug("event=job_done job={} conn={} key={} frame={} render_ms={:.1f} queued_ms={:.1f} out_bytes={} "
                  "cache_entries={} cache_mb={:.1f} evicted={}",
                  job->id, conn, job->key.hex(), job->req.frame_number, ms_since(t0), queued_ms, frame->data.size(),
                  cs.entries, cs.bytes / 1048576.0, evicted);
    finish(job, Status::Ok, frame, "");
  }
}

// -------------------------------------------------------------- stats ------
std::vector<std::pair<std::string, uint64_t>> Daemon::stats_pairs() {
  const auto cs = cache_.stats();
  uint64_t qj, qb, inf;
  {
    std::lock_guard<std::mutex> lk(qm_);
    qj = queue_.size();
    qb = queue_bytes_;
    inf = inflight_.size();
  }
  return {{"cache_entries", cs.entries},     {"cache_bytes", cs.bytes},       {"cache_target_bytes", cs.target_bytes},
          {"cache_hits", cs.hits},           {"cache_misses", cs.misses},     {"cache_evictions", cs.evictions},
          {"cache_too_large", cs.too_large}, {"queue_jobs", qj},              {"queue_bytes", qb},
          {"inflight_jobs", inf},            {"jobs_submitted", n_submitted_}, {"jobs_completed", n_completed_},
          {"jobs_skipped", n_skipped_},      {"jobs_rejected", n_rejected_},  {"jobs_deduplicated", n_dedup_},
          {"jobs_failed", n_job_errors_},    {"waiter_timeouts", n_waiter_timeouts_},
          {"workers", static_cast<uint64_t>(n_workers_.load())}, {"worker_connections", n_worker_connections_.load()}, {"filters", static_cast<uint64_t>(n_filters_.load())}};
}

std::string Daemon::stats_text() {
  std::ostringstream o;
  for (auto& [k, v] : stats_pairs()) o << k << "=" << v << "\n";
  return o.str();
}

std::string Daemon::stats_line() {
  std::ostringstream o;
  bool first = true;
  for (auto& [k, v] : stats_pairs()) {
    o << (first ? "" : " ") << k << "=" << v;
    first = false;
  }
  return o.str();
}

}  // namespace

// --------------------------------------------------------------- main ------
int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::puts("natron-kdenlive-daemon\n"
                "  Frame router between the Kdenlive MLT filter and the Natron worker.\n"
                "  Data directory: $NKB_HOME or ~/NatronKdenliveLink (config.ini, token, logs/)\n"
                "  Options: --help\n"
                "  Stop with Ctrl+C (SIGINT) or SIGTERM.");
      return 0;
    }
    std::fprintf(stderr, "unknown option %s (try --help)\n", argv[i]);
    return 2;
  }

  std::string err;
  if (!Config::write_default_if_missing(config_path(), &err) || !create_token_if_missing(token_path(), &err)) {
    std::fprintf(stderr, "setup failed: %s\n", err.c_str());
    return 1;
  }
  Config cfg;
  std::vector<std::string> warnings, errors;
  if (!cfg.load(config_path(), &warnings)) {
    std::fprintf(stderr, "cannot read %s\n", config_path().c_str());
    return 1;
  }
  if (!cfg.validate(&errors)) {
    std::fprintf(stderr, "invalid configuration in %s:\n", config_path().c_str());
    for (auto& e : errors) std::fprintf(stderr, "  %s\n", e.c_str());
    return 1;
  }
  std::string log_file = cfg.get("logging", "log_file");
  if (log_file.empty()) log_file = default_log_path();
  if (!init_logging("daemon", cfg.get("logging", "level"), log_file, cfg.get_bool("logging", "console"), &err))
    std::fprintf(stderr, "warning: %s\n", err.c_str());
  spdlog::info("event=config_loaded path={} log_file={}", config_path(), log_file);
  for (auto& w : warnings) spdlog::warn("event=config_warning detail=\"{}\"", w);

  std::string token;
  if (!read_token(token_path(), &token, &err)) {
    spdlog::error("event=startup_failed reason=\"{}\"", err);
    return 1;
  }

  struct sigaction sa{};
  sa.sa_handler = on_signal;  // no SA_RESTART: blocking calls return EINTR and loops re-check g_stop
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  Daemon d(cfg, token);
  if (!d.start(&err)) {
    spdlog::error("event=startup_failed reason=\"{}\"", err);
    return 1;
  }
  d.run();
  return 0;
}
