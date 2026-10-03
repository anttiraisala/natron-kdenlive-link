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
//   launcher_   "Open in Natron" (NatronLauncher, own mutex, own background threads)
//   supervisor_ starts the Natron worker and restarts it when it exits (own thread)
//   crashes_    per composition: crashes in a row, quarantine (guarded by crash_m_)
//
// CRASH PROTECTION
//   If the worker dies while rendering a job (the connection closes mid-job: Natron
//   crashed), the job's composition gets a strike. After [daemon] crash_limit strikes in
//   a row it is quarantined: its requests are answered with an error at once, so frames
//   pass through and the restarted worker is not killed again. The quarantine ends when
//   the composition's .ntp file changes (saved again) or the daemon restarts. A
//   composition is identified by the .ntp path in the request, or by its comp id.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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

std::vector<std::string> split_words(const std::string& s) {
  std::istringstream in(s);
  std::vector<std::string> v;
  for (std::string w; in >> w;) v.push_back(w);
  return v;
}

std::string join_words(const std::vector<std::string>& v) {
  std::string s;
  for (auto& w : v) s += (s.empty() ? "" : " ") + w;
  return s;
}

// Starts argv in a new session (so its process group can be stopped as a whole), with
// stdin from /dev/null and stdout/stderr appended to log_path. Returns the pid or -1.
pid_t spawn_process(const std::vector<std::string>& argv, const std::vector<std::string>& extra_env,
                    const std::string& log_path, std::string* err) {
  if (argv.empty()) {
    *err = "empty command";
    return -1;
  }
  std::vector<char*> args;
  for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  std::vector<std::string> env_store(extra_env);
  std::vector<char*> env;
  for (char** e = environ; *e; ++e) env.push_back(*e);
  for (auto& e : env_store) env.push_back(const_cast<char*>(e.c_str()));
  env.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  sigset_t none, def;
  sigemptyset(&none);
  sigemptyset(&def);
  sigaddset(&def, SIGPIPE);  // the daemon ignores SIGPIPE; Natron gets the default
  sigaddset(&def, SIGINT);
  sigaddset(&def, SIGTERM);
  posix_spawnattr_setsigmask(&at, &none);
  posix_spawnattr_setsigdefault(&at, &def);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  pid_t pid = -1;
  const int rc = posix_spawnp(&pid, args[0], &fa, &at, args.data(), env.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (rc != 0) {
    *err = std::string("cannot start ") + argv[0] + ": " + std::strerror(rc);
    return -1;
  }
  return pid;
}

// --------------------------------------------------------- Natron GUI ------
// "Open in Natron" in the Kdenlive effect arrives as the control command
// "open_natron <path of the .ntp>". The filter does not start Natron itself: it runs
// inside Kdenlive, whose AppImage sets library and Qt paths that would break another
// program (and a snap). The daemon was started from the user's terminal and has a
// clean environment.
//
// The GUI is started as [natron] gui_command -c <text of natron/nkb_gui_open.py> with
// NKB_OPEN=<path>, in its own session so it keeps running when the daemon stops. The
// script creates a missing composition (pass-through graph), shows the preview frame
// the filter saved and connects a viewer. A file that is already open in a GUI started
// by the daemon is not opened a second time; its window is brought to the front instead
// ([natron] raise_command <file name>, wmctrl by default: Natron's window title starts
// with the file name). Natron's output goes to <data dir>/logs/natron-gui.log.
class NatronLauncher {
 public:
  void configure(const Config& cfg) {
    gui_ = split_words(cfg.get("natron", "gui_command"));
    raise_ = split_words(cfg.get("natron", "raise_command"));
    scripts_dir_ = cfg.get("natron", "scripts_dir");
    if (scripts_dir_.empty()) scripts_dir_ = find_scripts_dir();
    log_path_ = home_dir() + "/logs/natron-gui.log";
    mkdir((home_dir() + "/logs").c_str(), 0755);  // may not exist when log_file points elsewhere
    spdlog::info("event=natron_launcher gui_command=\"{}\" raise_command=\"{}\" scripts_dir=\"{}\"",
                 join_words(gui_), join_words(raise_), scripts_dir_);
  }

  const std::string& scripts_dir() const { return scripts_dir_; }

  // Validates the request and starts the work in the background, so the control
  // reply (and with it Kdenlive's GUI thread) is never held up by Natron.
  // Returns false with *reply = reason for an invalid request.
  bool request(const std::string& path, uint64_t conn, std::string* reply) {
    if (path.empty() || path[0] != '/' || path.size() < 5 || path.compare(path.size() - 4, 4, ".ntp") != 0 ||
        path.find('\n') != std::string::npos) {
      *reply = "open_natron needs an absolute path of a .ntp file";
      spdlog::warn("event=natron_open_rejected conn={} path=\"{}\" reason=\"{}\"", conn, path, *reply);
      return false;
    }
    if (gui_.empty()) {
      *reply = "[natron] gui_command is empty in config.ini";
      spdlog::warn("event=natron_open_rejected conn={} path=\"{}\" reason=\"{}\"", conn, path, *reply);
      return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    if (auto it = open_.find(path); it != open_.end()) {
      *reply = "already_open pid=" + std::to_string(it->second);
      spdlog::info("event=natron_open_skipped conn={} path=\"{}\" reason=\"already open in Natron\" pid={}", conn,
                   path, it->second);
      if (it->second > 0) raise_window(path);
      return true;
    }
    open_[path] = 0;  // reserved while the composition is prepared
    spdlog::info("event=natron_open_requested conn={} path=\"{}\"", conn, path);
    std::thread([this, path] { work(path); }).detach();
    *reply = "accepted";
    return true;
  }

 private:
  static std::string find_scripts_dir() {
    std::vector<std::string> candidates;
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
      exe[n] = 0;
      std::string dir(exe);
      dir = dir.substr(0, dir.rfind('/'));
      candidates.push_back(dir + "/../natron");  // running from <source>/build
    }
    candidates.push_back(NKB_SOURCE_DIR "/natron");                    // the source tree it was built from
    candidates.push_back(NKB_INSTALL_DATADIR "/natron-kdenlive-link/natron");  // cmake --install
    for (auto& c : candidates) {
      struct stat st;
      if (stat((c + "/nkb_gui_open.py").c_str(), &st) == 0) return c;
    }
    return candidates.front();
  }

  // Runs raise_command <file name> in the background; its exit status says whether a
  // window was found (wmctrl: 0 = raised, 1 = no window with that title).
  void raise_window(const std::string& path) {
    if (raise_.empty()) return;
    std::vector<std::string> argv = raise_;
    argv.push_back(path.substr(path.rfind('/') + 1));
    std::string err;
    const pid_t pid = spawn_process(argv, {}, log_path_, &err);
    if (pid < 0) {
      spdlog::warn("event=natron_raise_failed path=\"{}\" reason=\"{}\" hint=\"sudo apt install wmctrl, or set "
                   "[natron] raise_command\"", path, err);
      return;
    }
    std::thread([pid, path, argv] {
      int status = 0;
      waitpid(pid, &status, 0);
      const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      if (rc == 0) spdlog::info("event=natron_raised path=\"{}\"", path);
      else spdlog::warn("event=natron_raise_failed path=\"{}\" status={} command=\"{}\"", path, rc, join_words(argv));
    }).detach();
  }

  void forget(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_);
    open_.erase(path);
  }

  void work(const std::string path) {
    // The GUI runs natron/nkb_gui_open.py, passed as text with -c so that a snap does not
    // need to read the file: it loads (or creates) the composition, points NKB_Input at the
    // preview frame and connects a viewer. Natron cannot run a script after loading a
    // project given on its command line, so the script loads it.
    std::string err;
    const std::string script_file = scripts_dir_ + "/nkb_gui_open.py";
    std::ifstream in(script_file);
    std::stringstream code;
    code << in.rdbuf();
    if (!in || code.str().empty()) {
      spdlog::error("event=natron_open_failed path=\"{}\" reason=\"cannot read {}\" hint=\"set [natron] scripts_dir\"",
                    path, script_file);
      return forget(path);
    }
    std::vector<std::string> argv = gui_;
    argv.push_back("-c");
    argv.push_back(code.str());
    const pid_t pid = spawn_process(argv, {"NKB_OPEN=" + path}, log_path_, &err);
    if (pid < 0) {
      spdlog::error("event=natron_open_failed path=\"{}\" reason=\"{}\"", path, err);
      return forget(path);
    }
    {
      std::lock_guard<std::mutex> lk(m_);
      open_[path] = pid;
    }
    spdlog::info("event=natron_gui_started path=\"{}\" pid={} command=\"{} -c <{}>\" output={}", path, pid,
                 join_words(gui_), script_file, log_path_);
    int status = 0;
    waitpid(pid, &status, 0);  // this thread lives as long as the Natron window
    spdlog::info("event=natron_gui_exited path=\"{}\" pid={} status={}", path, pid,
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    forget(path);
  }

  std::mutex m_;
  std::map<std::string, pid_t> open_;  // path -> pid of its Natron GUI (0 while being prepared)
  std::vector<std::string> gui_, raise_;
  std::string scripts_dir_, log_path_;
};

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

// ------------------------------------------------------ worker supervisor --
// With [natron] start_worker = true the daemon starts the Natron worker itself
// ([natron] worker_command -t natron/nkb_natron_worker.py) and starts it again whenever
// it exits: Natron can crash inside its own code (seen: a segfault in its TGA writer),
// which no Python code can catch. A worker that keeps dying soon after its start is
// restarted after 2, 4, 8 ... up to 60 s, so a composition that always crashes Natron
// does not cause a tight loop. Output (Natron messages, crash backtraces) goes to
// <data dir>/logs/natron-worker.log. When the daemon stops it stops the worker; the
// worker also gets NKB_EXIT_WITH_PID=<daemon pid> and leaves by itself if the daemon is
// killed (kill -9).
class WorkerSupervisor {
 public:
  void configure(const Config& cfg, const std::string& scripts_dir) {
    enabled_ = cfg.get_bool("natron", "start_worker");
    argv_ = split_words(cfg.get("natron", "worker_command"));
    argv_.push_back("-t");
    argv_.push_back(scripts_dir + "/nkb_natron_worker.py");
    log_path_ = home_dir() + "/logs/natron-worker.log";
    mkdir((home_dir() + "/logs").c_str(), 0755);
    if (enabled_)
      spdlog::info("event=worker_supervisor command=\"{}\" output={}", join_words(argv_), log_path_);
    else
      spdlog::info("event=worker_supervisor disabled=1 reason=\"[natron] start_worker = false; start the worker by hand\"");
  }

  void start() {
    if (enabled_) thread_ = std::thread([this] { loop(); });
  }

  // Called after g_stop is set: the loop stops the worker and returns.
  void join() {
    if (thread_.joinable()) thread_.join();
  }

 private:
  void loop() {
    int quick_failures = 0;
    while (!g_stop) {
      const auto t0 = Clock::now();
      std::string err;
      const pid_t pid = spawn_process(argv_, {"NKB_EXIT_WITH_PID=" + std::to_string(getpid())}, log_path_, &err);
      double ran_s = 0;
      if (pid < 0) {
        spdlog::error("event=worker_start_failed reason=\"{}\" command=\"{}\"", err, join_words(argv_));
      } else {
        spdlog::info("event=worker_launched pid={}", pid);
        int status = 0;
        bool exited = false;
        while (!g_stop) {
          if (waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!exited) {  // the daemon is stopping: stop the worker's whole process group
          kill(-pid, SIGTERM);
          for (int i = 0; i < 15 && waitpid(pid, &status, WNOHANG) != pid; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          kill(-pid, SIGKILL);  // Natron ignores SIGTERM
          waitpid(pid, &status, 0);
          spdlog::info("event=worker_stopped_by_daemon pid={}", pid);
          return;
        }
        ran_s = ms_since(t0) / 1000.0;
        const std::string how = WIFSIGNALED(status) ? "signal=" + std::to_string(WTERMSIG(status))
                                                    : "exit_code=" + std::to_string(WEXITSTATUS(status));
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
          // A clean exit is a deliberate stop (<data dir>/worker.stop, NKB_MAX_JOBS): respect it.
          spdlog::info("event=worker_exited pid={} {} ran_s={:.0f} restart=no reason=\"stopped on purpose\"", pid,
                       how, ran_s);
          return;
        }
        spdlog::warn("event=worker_exited pid={} {} ran_s={:.0f} restart=yes see={}", pid, how, ran_s, log_path_);
      }
      quick_failures = ran_s < 60 ? quick_failures + 1 : 1;
      const int delay_s = std::min(60, 2 << std::min(quick_failures - 1, 5));
      spdlog::info("event=worker_restarting in_s={} quick_failures={}", delay_s, quick_failures);
      for (int i = 0; i < delay_s * 5 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  bool enabled_ = false;
  std::vector<std::string> argv_;
  std::string log_path_;
  std::thread thread_;
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
  NatronLauncher launcher_;
  WorkerSupervisor supervisor_;

  // ---- crash protection
  struct CrashRecord {
    int in_a_row = 0;
    bool quarantined = false;
    std::string file;        // .ntp watched for changes
    int64_t mtime_ns = -1;   // of file when quarantined (-1 = missing)
  };
  static std::string comp_identity(const Header& h, const std::vector<uint8_t>& payload, std::string* file);
  static int64_t file_mtime_ns(const std::string& path);
  void record_crash(const std::shared_ptr<Job>& job, uint64_t conn);
  void record_success(const std::shared_ptr<Job>& job);
  bool is_quarantined(const std::string& id, std::string* why);
  uint64_t quarantined_count() {
    std::lock_guard<std::mutex> lk(crash_m_);
    uint64_t n = 0;
    for (auto& [id, c] : crashes_) n += c.quarantined ? 1 : 0;
    return n;
  }
  std::mutex crash_m_;
  std::map<std::string, CrashRecord> crashes_;
  int crash_limit_ = 3;
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
  launcher_.configure(cfg_);
  crash_limit_ = static_cast<int>(cfg_.get_int("daemon", "crash_limit"));
  supervisor_.configure(cfg_, launcher_.scripts_dir());

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
  supervisor_.start();  // the listeners are ready, so the worker can connect at once
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
  supervisor_.join();
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
  if (h.ntp_len > kMaxNtpLen || m.payload.size() != expected + h.ntp_len) {
    spdlog::warn("event=bad_request conn={} request_id={} reason=\"payload size mismatch\" got={} expected={} ntp_len={}",
                 conn, h.request_id, m.payload.size(), expected, h.ntp_len);
    return reply_status(s, h, Status::Error, "payload size does not match geometry");
  }

  spdlog::debug("event=cache_miss conn={} request_id={} comp={} frame={} key={} bytes={} timeout_ms={}", conn,
                h.request_id, comp, h.frame_number, key.hex(), m.payload.size(), timeout_ms);
  {
    std::string why;
    if (is_quarantined(comp_identity(h, m.payload, nullptr), &why)) return reply_status(s, h, Status::Error, why);
  }
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
  } else if (cmd.rfind("open_natron ", 0) == 0) {
    if (!launcher_.request(cmd.substr(12), conn, &out)) {
      Header h = make_header(MsgType::ControlReply);
      h.status = static_cast<uint16_t>(Status::Error);
      return send_message(s.fd(), h, out.data(), out.size(), nullptr);
    }
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
    {
      std::string why;  // queued before its composition was quarantined
      if (is_quarantined(comp_identity(job->req, *job->input, nullptr), &why)) {
        finish(job, Status::Error, nullptr, why);
        continue;
      }
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
      if (r == RecvResult::Closed) record_crash(job, conn);  // the worker died while rendering this job
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
    record_success(job);
    finish(job, Status::Ok, frame, "");
  }
}

// ---------------------------------------------------- crash protection -----
std::string Daemon::comp_identity(const Header& h, const std::vector<uint8_t>& payload, std::string* file) {
  const std::string ntp = payload_ntp_path(h, payload);
  if (!ntp.empty()) {
    if (file) *file = ntp;
    return ntp;
  }
  const std::string comp = get_comp_id(h);
  if (file) {  // the worker's default location of a comp without a path (same characters rule)
    std::string safe;
    for (unsigned char c : comp) safe += (std::isalnum(c) || c == '-' || c == '_' || c == '.') ? static_cast<char>(c) : '_';
    *file = home_dir() + "/comps/" + (safe.empty() ? "default" : safe) + ".ntp";
  }
  return "comp:" + comp;
}

int64_t Daemon::file_mtime_ns(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
}

void Daemon::record_crash(const std::shared_ptr<Job>& job, uint64_t conn) {
  if (crash_limit_ <= 0) return;
  std::string file;
  const std::string id = comp_identity(job->req, *job->input, &file);
  std::lock_guard<std::mutex> lk(crash_m_);
  CrashRecord& c = crashes_[id];
  ++c.in_a_row;
  spdlog::warn("event=comp_crashed comp=\"{}\" frame={} conn={} in_a_row={} limit={}", id, job->req.frame_number, conn,
               c.in_a_row, crash_limit_);
  if (c.in_a_row >= crash_limit_ && !c.quarantined) {
    c.quarantined = true;
    c.file = file;
    c.mtime_ns = file_mtime_ns(file);
    spdlog::error("event=comp_quarantined comp=\"{}\" crashes={} file=\"{}\" reason=\"the worker died {} times in a row "
                  "on this composition; its frames pass through until the file is saved again\"",
                  id, c.in_a_row, file, c.in_a_row);
  }
}

void Daemon::record_success(const std::shared_ptr<Job>& job) {
  if (crash_limit_ <= 0) return;
  const std::string id = comp_identity(job->req, *job->input, nullptr);
  std::lock_guard<std::mutex> lk(crash_m_);
  if (auto it = crashes_.find(id); it != crashes_.end() && !it->second.quarantined) crashes_.erase(it);
}

bool Daemon::is_quarantined(const std::string& id, std::string* why) {
  if (crash_limit_ <= 0) return false;
  std::lock_guard<std::mutex> lk(crash_m_);
  auto it = crashes_.find(id);
  if (it == crashes_.end() || !it->second.quarantined) return false;
  const int64_t now = file_mtime_ns(it->second.file);
  if (now != it->second.mtime_ns) {
    spdlog::info("event=comp_released comp=\"{}\" reason=\"file changed\"", id);
    crashes_.erase(it);
    return false;
  }
  *why = "composition " + id + " made Natron crash " + std::to_string(it->second.in_a_row) +
         " times in a row; save it again in Natron to retry";
  return true;
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
          {"workers", static_cast<uint64_t>(n_workers_.load())}, {"worker_connections", n_worker_connections_.load()}, {"filters", static_cast<uint64_t>(n_filters_.load())},
          {"comps_quarantined", quarantined_count()}};
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
  bool no_worker = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--no-worker") {
      no_worker = true;
      continue;
    }
    if (a == "--help" || a == "-h") {
      std::puts("natron-kdenlive-daemon\n"
                "  Frame router between the Kdenlive MLT filter and the Natron worker.\n"
                "  Data directory: $NKB_HOME or ~/NatronKdenliveLink (config.ini, token, logs/)\n"
                "  Options: --help\n"
                "           --no-worker   do not start the Natron worker ([natron] start_worker = false)\n"
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
  if (no_worker) cfg.set("natron", "start_worker", "false");
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
