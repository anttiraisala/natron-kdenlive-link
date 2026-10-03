// args.h - tiny "--name value" / "--flag" command line helper for the tools.
#pragma once
#include <cstdlib>
#include <map>
#include <string>

namespace nkbtools {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) != 0) { positional_ += (positional_.empty() ? "" : " ") + a; continue; }
      a = a.substr(2);
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) values_[a] = argv[++i];
      else values_[a] = "";
    }
  }
  bool has(const std::string& k) const { return values_.count(k) != 0; }
  std::string str(const std::string& k, const std::string& def = "") const {
    auto it = values_.find(k);
    return it == values_.end() ? def : it->second;
  }
  long num(const std::string& k, long def) const {
    auto it = values_.find(k);
    return it == values_.end() || it->second.empty() ? def : std::strtol(it->second.c_str(), nullptr, 10);
  }
 private:
  std::map<std::string, std::string> values_;
  std::string positional_;
};

}  // namespace nkbtools
