#pragma once

// Helpers shared by the HLE module implementations.

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "common/log.h"
#include "common/strings.h"
#include "hle/guest_types.h"
#include "hle/registry.h"
#include "runtime/process.h"

namespace xbem::hle {

inline rt::Process& proc() { return rt::Process::current(); }

inline void set_last_error(win::DWORD e) { rt::set_guest_last_error(e); }
inline win::DWORD last_error() { return rt::guest_last_error(); }

inline uint32_t current_tid() {
  rt::GuestThread* t = rt::current_thread();
  return t ? t->id : 0;
}

// Resolves a handle, including the pseudo-handle of the current thread.
inline std::shared_ptr<rt::KernelObject> lookup_handle(win::HANDLE h) {
  if (h == win::kCurrentThreadPseudo) return proc().current_thread_shared();
  return proc().handles().get(h);
}

// Ends the calling guest thread (ExitThread / _endthreadex).
[[noreturn]] void exit_guest_thread(uint32_t code);

// Copies `s` into a guest buffer of `capacity` characters (including NUL).
// Returns the number of characters written excluding NUL, or the required
// size including NUL if the buffer is too small (Win32 convention).
template <class Char>
uint32_t copy_out_string(std::basic_string_view<Char> s, Char* buffer, uint32_t capacity) {
  if (!buffer || capacity <= s.size()) return static_cast<uint32_t>(s.size() + 1);
  std::memcpy(buffer, s.data(), s.size() * sizeof(Char));
  buffer[s.size()] = 0;
  return static_cast<uint32_t>(s.size());
}

// Logs a message only the first time a given call site is reached.
#define XBEM_WARN_ONCE(cat, ...)                     \
  do {                                               \
    static std::atomic<bool> xbem_once_flag{false};  \
    if (!xbem_once_flag.exchange(true)) XBEM_WARN(cat, __VA_ARGS__); \
  } while (0)

// Wide/narrow guest string helpers.
inline std::string narrow(const char16_t* s) { return s ? utf16_to_utf8(guest_wstr(s)) : std::string(); }
inline std::string narrow(const char* s) { return std::string(guest_cstr(s)); }

}  // namespace xbem::hle
