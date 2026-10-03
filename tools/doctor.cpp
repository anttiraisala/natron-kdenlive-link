// natron-kdenlive-doctor - checks the installation and prints one line per
// check in a form that is easy to paste to a person or an AI:
//   check=<name> result=ok|warn|fail detail="..."
// Milestone 1 checks the data directory, config, token and the daemon.
// Later milestones add detection of Kdenlive/Natron install type (snap,
// flatpak, AppImage, apt) and the MLT plugin directory.
#include <filesystem>

#include <sys/stat.h>

#include "common.h"

using namespace nkb;

static int g_fail = 0;
static void report(const char* name, const char* result, const std::string& detail) {
  std::printf("check=%s result=%s detail=\"%s\"\n", name, result, detail.c_str());
  if (std::string(result) == "fail") ++g_fail;
}

int main() {
  namespace fs = std::filesystem;
  report("data_dir", fs::is_directory(home_dir()) ? "ok" : "fail", home_dir());

  nkbtools::Env env;
  std::vector<std::string> warnings, errors;
  if (!fs::exists(config_path())) {
    report("config", "warn", "no config.ini yet, defaults are used (the daemon writes one on first start)");
  } else if (!env.cfg.load(config_path(), &warnings)) {
    report("config", "fail", "cannot read " + config_path());
  } else if (!env.cfg.validate(&errors)) {
    for (auto& e : errors) report("config", "fail", e);
  } else {
    report("config", warnings.empty() ? "ok" : "warn", warnings.empty() ? config_path() : warnings.front());
  }

  std::string err;
  struct stat st;
  if (stat(token_path().c_str(), &st) != 0) {
    report("token", "fail", "missing " + token_path() + " (start the daemon once to create it)");
  } else {
    report("token", (st.st_mode & 077) ? "warn" : "ok",
           (st.st_mode & 077) ? "token file is readable by other users, run chmod 600" : token_path());
    read_token(token_path(), &env.token, &err);
  }

  for (const char* key : {"filter_address", "worker_address"}) {
    Address a;
    if (!Address::parse(env.cfg.get("daemon", key), &a, &err)) report(key, "fail", err);
    else report(key, "ok", a.str());
  }

  Address fa;
  if (Address::parse(env.cfg.get("daemon", "filter_address"), &fa, &err) && !env.token.empty()) {
    std::string reply;
    if (control_request(fa, env.token, "ping", &reply, &err)) {
      report("daemon", "ok", "answered ping");
      if (control_request(fa, env.token, "stats", &reply, &err)) {
        for (char& c : reply) if (c == '\n') c = ' ';
        report("daemon_stats", "ok", reply);
      }
    } else {
      report("daemon", "fail", "not reachable: " + err);
    }
  }
  return g_fail ? 1 : 0;
}
