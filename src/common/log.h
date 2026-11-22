#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace xbem::log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Off };

void set_level(Level level);
Level level();
bool parse_level(std::string_view text, Level& out);
const char* level_name(Level level);

inline bool enabled(Level lv) { return static_cast<int>(lv) >= static_cast<int>(level()); }

// Writes one line to stderr. Thread-safe.
void write(Level level, std::string_view category, std::string_view message);

// Text produced by the guest (OutputDebugString, console handles). Always goes
// to stdout, independent of the log level, so it can be piped and tested.
void guest_output(std::string_view text);

template <class... Args>
void logf(Level lv, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
  if (!enabled(lv)) return;
  write(lv, category, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace xbem::log

#define XBEM_TRACE(cat, ...) ::xbem::log::logf(::xbem::log::Level::Trace, cat, __VA_ARGS__)
#define XBEM_DEBUG(cat, ...) ::xbem::log::logf(::xbem::log::Level::Debug, cat, __VA_ARGS__)
#define XBEM_INFO(cat, ...) ::xbem::log::logf(::xbem::log::Level::Info, cat, __VA_ARGS__)
#define XBEM_WARN(cat, ...) ::xbem::log::logf(::xbem::log::Level::Warn, cat, __VA_ARGS__)
#define XBEM_ERROR(cat, ...) ::xbem::log::logf(::xbem::log::Level::Error, cat, __VA_ARGS__)
