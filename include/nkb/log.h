// log.h - spdlog setup. One structured line per event:
//   2026-10-02T10:15:23.456Z [debug] [daemon] [t=1234] event=frame_request key=... frame=42 ...
// Rules for log lines (so that an AI can read them):
//   * start with event=<snake_case_name>, then key=value pairs
//   * values never contain spaces (quote free text with "...")
//   * every error carries the reason (errno text or protocol detail)
#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

namespace nkb {

// Creates the default logger named `component`. level: trace|debug|info|warn|error.
// log_file may be empty (no file). Returns false and fills *err if the file
// cannot be opened (console logging still works).
bool init_logging(const std::string& component, const std::string& level,
                  const std::string& log_file, bool console, std::string* err);

}  // namespace nkb
