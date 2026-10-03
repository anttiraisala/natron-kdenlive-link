// common.h - shared start-up code for the command line tools: load config,
// read token, resolve an address.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "args.h"
#include "nkb/client.h"
#include "nkb/config.h"
#include "nkb/log.h"

namespace nkbtools {

struct Env {
  nkb::Config cfg;
  std::string token;
};

// Loads config (defaults if the file does not exist) and the token.
// `need_token` false lets `doctor` report problems instead of exiting.
inline bool load_env(Env* env, std::string* err, bool need_token = true) {
  std::vector<std::string> warnings, errors;
  env->cfg.load(nkb::config_path(), &warnings);  // missing file = defaults
  if (!env->cfg.validate(&errors)) {
    *err = "invalid configuration: " + (errors.empty() ? std::string() : errors.front());
    return false;
  }
  if (need_token && !nkb::read_token(nkb::token_path(), &env->token, err)) return false;
  return true;
}

inline std::string log_path(const Env& e) {
  std::string f = e.cfg.get("logging", "log_file");
  return f.empty() ? nkb::default_log_path() : f;
}

}  // namespace nkbtools
