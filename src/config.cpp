#include "nkb/config.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nkb {

// ---------------------------------------------------------------- paths ----
std::string home_dir() {
  if (const char* e = std::getenv("NKB_HOME"); e && *e) return e;
  // getpwuid, not $HOME: inside a snap $HOME points to a private directory.
  const passwd* pw = getpwuid(getuid());
  std::string base = pw && pw->pw_dir ? pw->pw_dir : "/tmp";
  return base + "/NatronKdenliveLink";
}
std::string config_path() { return home_dir() + "/config.ini"; }
std::string token_path() { return home_dir() + "/token"; }
std::string default_log_path() { return home_dir() + "/logs/natron-kdenlive.log"; }

// ------------------------------------------------------------ key table ----
namespace {

enum class Type { Str, Int, Bool, Enum };

struct Spec {
  const char* section;
  const char* key;
  const char* def;
  Type type;
  int64_t min;            // Int only
  int64_t max;            // Int only
  const char* choices;    // Enum only, '|' separated
  const char* comment;
};

// Single source of truth for known keys, defaults, ranges and documentation.
const Spec kSpecs[] = {
    {"daemon", "filter_address", "tcp:127.0.0.1:47801", Type::Str, 0, 0, "",
     "Where the daemon listens for Kdenlive filters and tools.\n"
     "# Syntax: tcp:127.0.0.1:PORT (works across snap/flatpak) or unix:/path/file.sock"},
    {"daemon", "worker_address", "tcp:127.0.0.1:47802", Type::Str, 0, 0, "",
     "Where the daemon listens for the Natron worker."},
    {"daemon", "cache_memory_mb", "1024", Type::Int, 1, 1048576, "",
     "Output cache (processed frames) target size in MB."},
    {"daemon", "cache_memory_min_mb", "256", Type::Int, 1, 1048576, "",
     "Lower clamp for cache_memory_mb."},
    {"daemon", "cache_memory_max_mb", "5120", Type::Int, 1, 1048576, "",
     "Upper clamp for cache_memory_mb."},
    {"daemon", "input_queue_memory_mb", "512", Type::Int, 1, 1048576, "",
     "Memory for frames waiting to be rendered by Natron."},
    {"daemon", "buffer_behavior", "buffer_skip", Type::Enum, 0, 0, "pause|buffer_skip|show_cached",
     "What happens when the input queue is full:\n"
     "#   pause        the requesting filter waits for free space (up to its timeout)\n"
     "#   buffer_skip  the oldest queued frame is dropped to make room\n"
     "#   show_cached  the new request is rejected; the filter passes its frame through"},
    {"daemon", "natron_timeout_seconds", "10", Type::Int, 1, 3600, "",
     "A worker that needs longer than this for one frame is declared stuck and dropped."},
    {"daemon", "default_request_timeout_ms", "1000", Type::Int, 0, 600000, "",
     "How long a filter request may wait for a render when the filter does not say."},
    {"daemon", "no_worker_wait_ms", "0", Type::Int, 0, 600000, "",
     "While no Natron worker is connected, a request waits at most this long (0 = answer\n"
     "# immediately with pass-through). Frames are still queued and rendered when a worker connects."},
    {"daemon", "crash_limit", "3", Type::Int, 0, 100, "",
     "A composition that made the worker die this many times in a row gets no more frames (they pass\n"
     "# through unprocessed) until its .ntp file changes, i.e. until it is saved again. 0 = off."},
    {"daemon", "max_frame_mb", "1024", Type::Int, 1, 65536, "",
     "Largest single frame accepted (protects against corrupt headers)."},
    {"daemon", "stats_log_interval_seconds", "5", Type::Int, 0, 3600, "",
     "Log a stats line every N seconds. 0 disables."},
    {"natron", "start_worker", "true", Type::Bool, 0, 0, "",
     "Start the Natron worker with the daemon and start it again whenever it exits (Natron can crash).\n"
     "# false = start nkb_natron_worker.py by hand."},
    {"natron", "worker_command", "snap run natron", Type::Str, 0, 0, "",
     "Command that runs the worker inside Natron; \"-t <scripts_dir>/nkb_natron_worker.py\" is added.\n"
     "# Tarball example: /home/me/apps/natron/Natron-2.5.0-Linux-x86_64-no-installer/NatronRenderer"},
    {"natron", "gui_command", "snap run natron", Type::Str, 0, 0, "",
     "Command that starts the Natron GUI; \"-c <script>\" is added, the script opens the .ntp.\n"
     "# Used by \"Open in Natron\" in the Kdenlive effect. Words are split at spaces (no quoting).\n"
     "# Tarball example: /home/me/apps/natron/Natron-2.5.0-Linux-x86_64-no-installer/Natron"},
    {"natron", "raise_command", "wmctrl -a", Type::Str, 0, 0, "",
     "Brings an already open Natron window to the front when \"Open in Natron\" is clicked again;\n"
     "# the file name (e.g. comp-abc123.ntp, part of Natron's window title) is added as the last\n"
     "# argument. wmctrl: sudo apt install wmctrl (X11 desktops). Empty = do not raise."},
    {"natron", "scripts_dir", "", Type::Str, 0, 0, "",
     "Folder of nkb_gui_open.py and nkb_natron_worker.py. Empty = found automatically: the natron/\n"
     "# tree when the daemon runs from build/, or the installed share/natron-kdenlive-link/natron."},
    {"logging", "level", "debug", Type::Enum, 0, 0, "trace|debug|info|warn|error",
     "Log verbosity. debug logs every frame event."},
    {"logging", "log_file", "", Type::Str, 0, 0, "",
     "Empty = <data dir>/logs/natron-kdenlive.log"},
    {"logging", "console", "true", Type::Bool, 0, 0, "", "Also log to the terminal."},
};

const Spec* find_spec(const std::string& section, const std::string& key) {
  for (const auto& s : kSpecs)
    if (section == s.section && key == s.key) return &s;
  return nullptr;
}

std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

bool parse_int(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (*end != '\0') return false;
  *out = v;
  return true;
}

bool parse_bool(const std::string& s, bool* out) {
  if (s == "true" || s == "1" || s == "yes" || s == "on") { *out = true; return true; }
  if (s == "false" || s == "0" || s == "no" || s == "off") { *out = false; return true; }
  return false;
}

bool in_choices(const std::string& v, const std::string& choices) {
  std::stringstream ss(choices);
  std::string c;
  while (std::getline(ss, c, '|'))
    if (c == v) return true;
  return false;
}

}  // namespace

// --------------------------------------------------------------- Config ----
Config::Config() {
  for (const auto& s : kSpecs) values_[std::string(s.section) + "." + s.key] = s.def;
}

bool Config::load(const std::string& path, std::vector<std::string>* warnings) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line, section;
  int n = 0;
  while (std::getline(in, line)) {
    ++n;
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      if (warnings) warnings->push_back(path + ":" + std::to_string(n) + ": not a key = value line");
      continue;
    }
    const std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (!find_spec(section, key)) {
      if (warnings) warnings->push_back(path + ":" + std::to_string(n) + ": unknown key [" + section + "] " + key);
      continue;
    }
    values_[section + "." + key] = val;
  }
  return true;
}

bool Config::validate(std::vector<std::string>* errors) const {
  bool ok = true;
  for (const auto& s : kSpecs) {
    const std::string v = get(s.section, s.key);
    const std::string name = std::string("[") + s.section + "] " + s.key;
    auto fail = [&](const std::string& why) {
      ok = false;
      if (errors) errors->push_back(name + " = \"" + v + "\": " + why);
    };
    switch (s.type) {
      case Type::Int: {
        int64_t i;
        if (!parse_int(v, &i)) fail("not an integer");
        else if (i < s.min || i > s.max)
          fail("out of range " + std::to_string(s.min) + ".." + std::to_string(s.max));
        break;
      }
      case Type::Bool: {
        bool b;
        if (!parse_bool(v, &b)) fail("not a boolean (true/false)");
        break;
      }
      case Type::Enum:
        if (!in_choices(v, s.choices)) fail(std::string("must be one of ") + s.choices);
        break;
      case Type::Str: break;
    }
  }
  return ok;
}

std::string Config::get(const std::string& section, const std::string& key) const {
  auto it = values_.find(section + "." + key);
  return it == values_.end() ? "" : it->second;
}

int64_t Config::get_int(const std::string& section, const std::string& key) const {
  int64_t v = 0;
  parse_int(get(section, key), &v);
  return v;
}

bool Config::get_bool(const std::string& section, const std::string& key) const {
  bool v = false;
  parse_bool(get(section, key), &v);
  return v;
}

void Config::set(const std::string& section, const std::string& key, const std::string& value) {
  values_[section + "." + key] = value;
}

std::string Config::default_file_text() {
  std::ostringstream o;
  o << "# natron-kdenlive-link configuration\n"
       "# Changes need a daemon restart. Whole-line comments only.\n";
  std::string last;
  for (const auto& s : kSpecs) {
    if (last != s.section) {
      o << "\n[" << s.section << "]\n";
      last = s.section;
    }
    o << "\n# " << s.comment << "\n";
    if (s.type == Type::Int) o << "# range " << s.min << ".." << s.max << "\n";
    o << s.key << " = " << s.def << "\n";
  }
  return o.str();
}

bool Config::write_default_if_missing(const std::string& path, std::string* err) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::exists(path, ec)) return true;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream out(path);
  if (!out) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  out << default_file_text();
  return true;
}

// ---------------------------------------------------------------- token ----
bool create_token_if_missing(const std::string& path, std::string* err) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::exists(path, ec)) return true;
  fs::create_directories(fs::path(path).parent_path(), ec);
  unsigned char raw[16];
  FILE* r = std::fopen("/dev/urandom", "rb");
  if (!r || std::fread(raw, 1, sizeof raw, r) != sizeof raw) {
    if (r) std::fclose(r);
    if (err) *err = "cannot read /dev/urandom";
    return false;
  }
  std::fclose(r);
  char hex[33];
  for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", raw[i]);
  const mode_t old = umask(077);  // created 0600
  std::ofstream out(path);
  umask(old);
  if (!out) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  out << hex << "\n";
  return true;
}

bool read_token(const std::string& path, std::string* token, std::string* err) {
  std::ifstream in(path);
  if (!in) {
    if (err) *err = "cannot read token file " + path + " (is the daemon set up for this user?)";
    return false;
  }
  std::getline(in, *token);
  *token = trim(*token);
  if (token->empty()) {
    if (err) *err = "token file is empty: " + path;
    return false;
  }
  return true;
}

}  // namespace nkb
