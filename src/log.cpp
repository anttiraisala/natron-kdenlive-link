#include "nkb/log.h"

#include <filesystem>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace nkb {

bool init_logging(const std::string& component, const std::string& level,
                  const std::string& log_file, bool console, std::string* err) {
  bool ok = true;
  std::vector<spdlog::sink_ptr> sinks;
  if (console) sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
  if (!log_file.empty()) {
    try {
      std::filesystem::create_directories(std::filesystem::path(log_file).parent_path());
      sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, false));
    } catch (const std::exception& e) {
      if (err) *err = std::string("cannot open log file ") + log_file + ": " + e.what();
      ok = false;
    }
  }
  if (sinks.empty()) sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
  auto logger = std::make_shared<spdlog::logger>(component, sinks.begin(), sinks.end());
  // UTC ISO-8601 timestamps with milliseconds, level, component, thread id.
  logger->set_formatter(std::make_unique<spdlog::pattern_formatter>(
      "%Y-%m-%dT%H:%M:%S.%eZ [%^%l%$] [%n] [t=%t] %v", spdlog::pattern_time_type::utc));
  logger->set_level(spdlog::level::from_str(level));
  // Development phase: flush every line so a crash never loses the last events.
  logger->flush_on(spdlog::level::trace);
  spdlog::set_default_logger(logger);
  return ok;
}

}  // namespace nkb
