// nkb-mock-worker - stands in for the Natron worker so the daemon can be
// tested without Natron. Applies a trivial transform to every job.
//
//   --mode passthrough|invert   invert flips the R,G,B bytes of RGBA8 frames
//   --delay-ms N                simulated render time per frame
//   --fail-frame N              reply with a worker error for frame N
//   --hang-frame N              never answer frame N (tests the worker timeout)
//   --exit-after N              disconnect after N jobs (tests reconnect handling)
//   --address A                 override worker_address from config.ini
#include <chrono>
#include <thread>

#include "common.h"

using namespace nkb;

int main(int argc, char** argv) {
  nkbtools::Args args(argc, argv);
  nkbtools::Env env;
  std::string err;
  if (!nkbtools::load_env(&env, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  init_logging("mock-worker", env.cfg.get("logging", "level"), nkbtools::log_path(env),
               env.cfg.get_bool("logging", "console"), nullptr);

  Address addr;
  if (!Address::parse(args.str("address", env.cfg.get("daemon", "worker_address")), &addr, &err)) {
    spdlog::error("event=bad_address reason=\"{}\"", err);
    return 1;
  }
  const std::string mode = args.str("mode", "invert");
  const long delay_ms = args.num("delay-ms", 0);
  const long fail_frame = args.num("fail-frame", -1);
  const long hang_frame = args.num("hang-frame", -1);
  const long exit_after = args.num("exit-after", -1);

  Socket s = connect_and_handshake(addr, Role::Worker, env.token, 3000, &err);
  if (!s.valid()) {
    spdlog::error("event=connect_failed address={} reason=\"{}\"", addr.str(), err);
    return 1;
  }
  spdlog::info("event=worker_connected address={} mode={} delay_ms={}", addr.str(), mode, delay_ms);

  long jobs = 0;
  while (true) {
    Message m;
    const RecvResult r = recv_message(s.fd(), m, 1000, 1ull << 32, &err);
    if (r == RecvResult::Timeout) continue;
    if (r != RecvResult::Ok) {
      spdlog::info("event=worker_disconnected reason=\"{}\"", r == RecvResult::Closed ? "daemon closed connection" : err);
      return 0;
    }
    if (m.h.type != static_cast<uint16_t>(MsgType::Job)) continue;
    const auto t0 = std::chrono::steady_clock::now();
    spdlog::debug("event=job_received job={} frame={} bytes={}", m.h.request_id, m.h.frame_number, m.payload.size());

    if (m.h.frame_number == hang_frame) {
      spdlog::warn("event=hanging frame={}", hang_frame);
      std::this_thread::sleep_for(std::chrono::hours(1));
    }
    if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    Header h = m.h;
    h.type = static_cast<uint16_t>(MsgType::JobResult);
    if (m.h.frame_number == fail_frame) {
      const std::string msg = "simulated worker failure";
      h.status = static_cast<uint16_t>(Status::Error);
      send_message(s.fd(), h, msg.data(), msg.size(), nullptr);
      continue;
    }
    if (mode == "invert" && m.h.pixel_format == static_cast<uint16_t>(PixelFormat::RGBA8)) {
      for (size_t i = 0; i + 3 < m.payload.size(); i += 4) {
        m.payload[i] = 255 - m.payload[i];
        m.payload[i + 1] = 255 - m.payload[i + 1];
        m.payload[i + 2] = 255 - m.payload[i + 2];
      }
    }
    h.status = static_cast<uint16_t>(Status::Ok);
    if (!send_message(s.fd(), h, m.payload.data(), m.payload.size(), &err)) {
      spdlog::error("event=send_failed reason=\"{}\"", err);
      return 1;
    }
    spdlog::debug("event=job_replied job={} frame={} render_ms={:.1f}", m.h.request_id, m.h.frame_number,
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    if (++jobs == exit_after) {
      spdlog::info("event=worker_exiting reason=\"exit-after reached\" jobs={}", jobs);
      return 0;
    }
  }
}
