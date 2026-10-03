// config.h - INI configuration, data directory handling and the auth token.
//
// Data directory (holds config.ini, token, logs/):
//   1. $NKB_HOME if set
//   2. <home from getpwuid>/NatronKdenliveLink   (NOT $HOME and not a hidden
//      folder, because sandboxed apps cannot read hidden folders of the real home)
//
// INI rules: "[section]" headers, "key = value" lines, '#' or ';' start a
// comment line (whole line only, inline comments are not supported so paths
// may contain '#'). A value may be wrapped in double quotes.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nkb {

std::string home_dir();
std::string config_path();
std::string token_path();
std::string default_log_path();

class Config {
 public:
  // Starts with every key at its default value.
  Config();

  // Reads an INI file over the defaults. Unknown keys and syntax problems are
  // appended to `warnings`; returns false only if the file cannot be read.
  bool load(const std::string& path, std::vector<std::string>* warnings);
  // Checks types and ranges of all known keys. Returns true if all are valid.
  bool validate(std::vector<std::string>* errors) const;

  std::string get(const std::string& section, const std::string& key) const;
  int64_t get_int(const std::string& section, const std::string& key) const;
  bool get_bool(const std::string& section, const std::string& key) const;
  void set(const std::string& section, const std::string& key, const std::string& value);

  // Fully commented default config file text.
  static std::string default_file_text();
  // Writes the default config if `path` does not exist. Creates directories.
  static bool write_default_if_missing(const std::string& path, std::string* err);

 private:
  std::map<std::string, std::string> values_;  // "section.key" -> value
};

// Creates the token file (random 128 bit hex, mode 0600) if missing.
bool create_token_if_missing(const std::string& path, std::string* err);
bool read_token(const std::string& path, std::string* token, std::string* err);

}  // namespace nkb
