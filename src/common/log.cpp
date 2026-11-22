#include "common/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "common/strings.h"

namespace xbem::log {
namespace {
std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mutex;
}  // namespace

void set_level(Level lv) { g_level.store(static_cast<int>(lv)); }
Level level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

const char* level_name(Level lv) {
  switch (lv) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Off: return "off";
  }
  return "?";
}

bool parse_level(std::string_view text, Level& out) {
  const std::string t = to_lower(text);
  for (Level lv : {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Off}) {
    if (t == level_name(lv)) {
      out = lv;
      return true;
    }
  }
  return false;
}

void write(Level lv, std::string_view category, std::string_view message) {
  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "[%-5s][%.*s] %.*s\n", level_name(lv), static_cast<int>(category.size()),
               category.data(), static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

void guest_output(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
  std::lock_guard lock(g_mutex);
  std::fprintf(stdout, "[guest] %.*s\n", static_cast<int>(text.size()), text.data());
  std::fflush(stdout);
}

}  // namespace xbem::log
