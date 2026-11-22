// HLE implementation of the console kernel DLL (kernelx.dll on Xbox, the
// equivalent of kernel32/kernelbase on PC) - core part: debugging, time,
// errors, process info, modules, strings/locale, system information.
//
// Synchronisation/threads, memory and files live in kernelx_sync.cpp,
// kernelx_memory.cpp and kernelx_file.cpp.

#include <chrono>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "hle/hle_util.h"
#include "loader/pe_format.h"
#include "platform/cpu.h"
#include "platform/thread.h"

namespace xbem::hle {
namespace {

using namespace win;
using rt::LoadedModule;

// ---------------------------------------------------------------- debugging

void XBEM_GUEST_ABI hle_OutputDebugStringA(LPCSTR s) {
  if (s) log::guest_output(guest_cstr(s));
}

void XBEM_GUEST_ABI hle_OutputDebugStringW(LPCWSTR s) {
  if (s) log::guest_output(narrow(s));
}

BOOL XBEM_GUEST_ABI hle_IsDebuggerPresent() { return FALSE; }

void XBEM_GUEST_ABI hle_DebugBreak() { XBEM_WARN("kernelx", "guest called DebugBreak() - ignored"); }

// --------------------------------------------------------------------- time

constexpr int64_t kQpcFrequency = 10'000'000;               // 100 ns ticks, like Windows 10+
constexpr uint64_t kUnixEpochAsFileTime = 116444736000000000ull;  // 1601 -> 1970 in 100 ns

uint64_t steady_100ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
}

uint64_t filetime_now() {
  using namespace std::chrono;
  const auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  return kUnixEpochAsFileTime + static_cast<uint64_t>(ns / 100);
}

DWORD XBEM_GUEST_ABI hle_GetTickCount() { return static_cast<DWORD>(steady_100ns() / 10000); }
ULONGLONG XBEM_GUEST_ABI hle_GetTickCount64() { return steady_100ns() / 10000; }

BOOL XBEM_GUEST_ABI hle_QueryPerformanceCounter(LONGLONG* out) {
  if (!out) return FALSE;
  *out = static_cast<LONGLONG>(steady_100ns());
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_QueryPerformanceFrequency(LONGLONG* out) {
  if (!out) return FALSE;
  *out = kQpcFrequency;
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_QueryUnbiasedInterruptTime(ULONGLONG* out) {
  if (!out) return FALSE;
  *out = steady_100ns();
  return TRUE;
}

void XBEM_GUEST_ABI hle_GetSystemTimeAsFileTime(void* ft) {
  const uint64_t v = filetime_now();
  std::memcpy(ft, &v, 8);  // FILETIME is only 4-byte aligned
}

void XBEM_GUEST_ABI hle_GetSystemTimePreciseAsFileTime(void* ft) { hle_GetSystemTimeAsFileTime(ft); }

void fill_systemtime(void* out, bool local) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t tt = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
#if defined(_WIN32)
  if (local) localtime_s(&tm, &tt); else gmtime_s(&tm, &tt);
#else
  if (local) localtime_r(&tt, &tm); else gmtime_r(&tt, &tm);
#endif
  const WORD st[8] = {static_cast<WORD>(1900 + tm.tm_year), static_cast<WORD>(tm.tm_mon + 1),
                      static_cast<WORD>(tm.tm_wday),        static_cast<WORD>(tm.tm_mday),
                      static_cast<WORD>(tm.tm_hour),        static_cast<WORD>(tm.tm_min),
                      static_cast<WORD>(tm.tm_sec),         static_cast<WORD>(ms)};
  std::memcpy(out, st, sizeof st);
}

void XBEM_GUEST_ABI hle_GetSystemTime(void* st) { fill_systemtime(st, false); }
void XBEM_GUEST_ABI hle_GetLocalTime(void* st) { fill_systemtime(st, true); }

void XBEM_GUEST_ABI hle_Sleep(DWORD ms) {
  if (ms == 0) std::this_thread::yield();
  else if (ms != INFINITE) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  else for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

DWORD XBEM_GUEST_ABI hle_SleepEx(DWORD ms, BOOL /*alertable*/) {
  hle_Sleep(ms);
  return 0;
}

BOOL XBEM_GUEST_ABI hle_SwitchToThread() {
  std::this_thread::yield();
  return TRUE;
}

// ------------------------------------------------------------------- errors

DWORD XBEM_GUEST_ABI hle_GetLastError() { return last_error(); }
void XBEM_GUEST_ABI hle_SetLastError(DWORD e) { set_last_error(e); }

// ------------------------------------------------------------------ process

HANDLE XBEM_GUEST_ABI hle_GetCurrentProcess() { return kCurrentProcessPseudo; }
HANDLE XBEM_GUEST_ABI hle_GetCurrentThread() { return kCurrentThreadPseudo; }
DWORD XBEM_GUEST_ABI hle_GetCurrentProcessId() { return proc().process_id(); }
DWORD XBEM_GUEST_ABI hle_GetCurrentThreadId() { return current_tid(); }
DWORD XBEM_GUEST_ABI hle_GetCurrentProcessorNumber() { return platform::current_processor_number(); }

void XBEM_GUEST_ABI hle_ExitProcess(UINT code) {
  XBEM_INFO("kernelx", "ExitProcess({})", code);
  proc().exit_process(code);
}

BOOL XBEM_GUEST_ABI hle_TerminateProcess(HANDLE h, UINT code) {
  if (h == kCurrentProcessPseudo) {
    XBEM_INFO("kernelx", "TerminateProcess(self, {})", code);
    proc().exit_process(code);
  }
  set_last_error(ERROR_INVALID_HANDLE);
  return FALSE;
}

// ------------------------------------------------------------------ modules

LoadedModule* module_from_handle(HMODULE h) {
  return h ? proc().loader().find_by_handle(h) : proc().loader().main_module();
}

HMODULE get_module_handle(const std::string* name) {
  LoadedModule* m = name ? proc().loader().find_loaded(*name) : proc().loader().main_module();
  if (!m) {
    set_last_error(ERROR_MOD_NOT_FOUND);
    return 0;
  }
  return m->handle();
}

HMODULE XBEM_GUEST_ABI hle_GetModuleHandleA(LPCSTR name) {
  if (!name) return get_module_handle(nullptr);
  const std::string n = narrow(name);
  return get_module_handle(&n);
}

HMODULE XBEM_GUEST_ABI hle_GetModuleHandleW(LPCWSTR name) {
  if (!name) return get_module_handle(nullptr);
  const std::string n = narrow(name);
  return get_module_handle(&n);
}

constexpr DWORD kGetModuleHandleExFromAddress = 0x4;

BOOL get_module_handle_ex(DWORD flags, const void* name, bool wide, HMODULE* out) {
  if (!out) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  LoadedModule* m = nullptr;
  if (flags & kGetModuleHandleExFromAddress) m = proc().loader().find_containing(reinterpret_cast<uint64_t>(name));
  else if (!name) m = proc().loader().main_module();
  else m = proc().loader().find_loaded(wide ? narrow(static_cast<LPCWSTR>(name)) : narrow(static_cast<LPCSTR>(name)));
  *out = m ? m->handle() : 0;
  if (!m) {
    set_last_error(ERROR_MOD_NOT_FOUND);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
  return get_module_handle_ex(flags, name, false, out);
}
BOOL XBEM_GUEST_ABI hle_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out) {
  return get_module_handle_ex(flags, name, true, out);
}

uint64_t XBEM_GUEST_ABI hle_GetProcAddress(HMODULE h, LPCSTR name) {
  LoadedModule* m = module_from_handle(h);
  if (!m) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  const auto raw = reinterpret_cast<uint64_t>(name);
  uint64_t addr;
  std::string what;
  if (raw < 0x10000) {
    addr = proc().loader().get_proc_address(*m, static_cast<uint16_t>(raw));
    what = "#" + std::to_string(raw);
  } else {
    what = narrow(name);
    addr = proc().loader().get_proc_address(*m, what);
  }
  if (!addr) {
    static std::mutex mu;
    static std::set<std::string> reported;
    std::lock_guard lock(mu);
    if (reported.insert(m->name + "!" + what).second)
      XBEM_WARN("kernelx", "GetProcAddress({}, {}) -> not available, returning NULL", m->name, what);
    set_last_error(ERROR_PROC_NOT_FOUND);
  }
  return addr;
}

HMODULE load_library(const std::string& name) {
  rt::Process& p = proc();
  LoadedModule* m = nullptr;
  try {
    const bool has_path = name.find_first_of("\\/:") != std::string::npos;
    const std::string file = name.substr(name.find_last_of("\\/:") + 1);
    if (Registry::instance().find_module(file)) {
      m = p.loader().load_library(file);  // system DLL requested by full path
    } else if (has_path) {
      if (auto r = p.vfs().resolve(std::string_view(name))) m = p.loader().load_library_path(r->host_path);
    } else {
      m = p.loader().load_library(name);
    }
  } catch (const std::exception& e) {
    XBEM_ERROR("kernelx", "LoadLibrary({}) failed: {}", name, e.what());
  }
  if (!m) {
    XBEM_WARN("kernelx", "LoadLibrary({}) -> not found", name);
    set_last_error(ERROR_MOD_NOT_FOUND);
    return 0;
  }
  p.initialize_pending_modules();
  return m->handle();
}

HMODULE XBEM_GUEST_ABI hle_LoadLibraryA(LPCSTR name) { return load_library(narrow(name)); }
HMODULE XBEM_GUEST_ABI hle_LoadLibraryW(LPCWSTR name) { return load_library(narrow(name)); }
HMODULE XBEM_GUEST_ABI hle_LoadLibraryExA(LPCSTR name, HANDLE, DWORD) { return load_library(narrow(name)); }
HMODULE XBEM_GUEST_ABI hle_LoadLibraryExW(LPCWSTR name, HANDLE, DWORD) { return load_library(narrow(name)); }
BOOL XBEM_GUEST_ABI hle_FreeLibrary(HMODULE) { return TRUE; }  // modules are never unloaded

BOOL XBEM_GUEST_ABI hle_DisableThreadLibraryCalls(HMODULE h) {
  if (LoadedModule* m = module_from_handle(h)) {
    m->thread_calls_disabled = true;
    return TRUE;
  }
  set_last_error(ERROR_INVALID_HANDLE);
  return FALSE;
}

std::string module_guest_path(LoadedModule& m) {
  if (m.host_path.empty()) return m.name;
  return proc().vfs().to_guest(m.host_path).value_or(m.name);
}

template <class Char>
DWORD get_module_file_name(HMODULE h, Char* buffer, DWORD size) {
  LoadedModule* m = module_from_handle(h);
  if (!m) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  std::basic_string<Char> path;
  if constexpr (sizeof(Char) == 2) path = utf8_to_utf16(module_guest_path(*m));
  else path = module_guest_path(*m);
  if (!buffer || size == 0) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  if (path.size() >= size) {  // truncated, still NUL-terminated
    std::memcpy(buffer, path.data(), (size - 1) * sizeof(Char));
    buffer[size - 1] = 0;
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return size;
  }
  std::memcpy(buffer, path.data(), path.size() * sizeof(Char));
  buffer[path.size()] = 0;
  return static_cast<DWORD>(path.size());
}

DWORD XBEM_GUEST_ABI hle_GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD size) { return get_module_file_name(h, buf, size); }
DWORD XBEM_GUEST_ABI hle_GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD size) { return get_module_file_name(h, buf, size); }

// ------------------------------------------------------------ strings/locale

constexpr UINT CP_ACP = 0;
constexpr UINT CP_OEMCP = 1;
constexpr UINT CP_UTF8 = 65001;

bool is_utf8_codepage(UINT cp) { return cp == CP_UTF8 || cp == CP_ACP || cp == CP_OEMCP || cp == 3 /*CP_THREAD_ACP*/; }

INT XBEM_GUEST_ABI hle_MultiByteToWideChar(UINT cp, DWORD, LPCSTR src, INT src_len, LPWSTR dst, INT dst_len) {
  if (!src || src_len == 0 || dst_len < 0) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  const size_t n = src_len < 0 ? bounded_strlen(src) + 1 : static_cast<size_t>(src_len);
  std::u16string w;
  if (is_utf8_codepage(cp)) {
    w = utf8_to_utf16(std::string_view(src, n));
  } else {
    if (cp != 1252 && cp != 28591 && cp != 20127) XBEM_WARN_ONCE("kernelx", "code page {} treated as Latin-1", cp);
    for (size_t i = 0; i < n; ++i) w.push_back(static_cast<char16_t>(static_cast<uint8_t>(src[i])));
  }
  if (dst_len == 0) return static_cast<INT>(w.size());
  if (!dst || w.size() > static_cast<size_t>(dst_len)) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  std::memcpy(dst, w.data(), w.size() * 2);
  return static_cast<INT>(w.size());
}

INT XBEM_GUEST_ABI hle_WideCharToMultiByte(UINT cp, DWORD, LPCWSTR src, INT src_len, LPSTR dst, INT dst_len,
                                           LPCSTR, BOOL* used_default) {
  if (!src || src_len == 0 || dst_len < 0) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  const size_t n = src_len < 0 ? bounded_strlen16(src) + 1 : static_cast<size_t>(src_len);
  std::string out;
  if (used_default) *used_default = FALSE;
  if (is_utf8_codepage(cp)) {
    out = utf16_to_utf8(std::u16string_view(src, n));
  } else {
    for (size_t i = 0; i < n; ++i) {
      if (src[i] > 0xFF) {
        out.push_back('?');
        if (used_default) *used_default = TRUE;
      } else {
        out.push_back(static_cast<char>(src[i]));
      }
    }
  }
  if (dst_len == 0) return static_cast<INT>(out.size());
  if (!dst || out.size() > static_cast<size_t>(dst_len)) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  std::memcpy(dst, out.data(), out.size());
  return static_cast<INT>(out.size());
}

INT XBEM_GUEST_ABI hle_lstrlenA(LPCSTR s) { return s ? static_cast<INT>(bounded_strlen(s)) : 0; }
INT XBEM_GUEST_ABI hle_lstrlenW(LPCWSTR s) { return s ? static_cast<INT>(bounded_strlen16(s)) : 0; }

UINT XBEM_GUEST_ABI hle_GetACP() { return CP_UTF8; }
UINT XBEM_GUEST_ABI hle_GetOEMCP() { return CP_UTF8; }

BOOL XBEM_GUEST_ABI hle_IsValidCodePage(UINT cp) {
  return (is_utf8_codepage(cp) || cp == 1252 || cp == 437 || cp == 20127 || cp == 28591) ? TRUE : FALSE;
}

BOOL XBEM_GUEST_ABI hle_GetCPInfo(UINT cp, uint8_t* info) {
  if (!info || !hle_IsValidCodePage(cp)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  std::memset(info, 0, 20);  // CPINFO: UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]
  const UINT max_char = is_utf8_codepage(cp) ? 4 : 1;
  std::memcpy(info, &max_char, 4);
  info[4] = '?';
  return TRUE;
}

constexpr DWORD kLocaleEnUs = 0x409;
DWORD XBEM_GUEST_ABI hle_GetUserDefaultLCID() { return kLocaleEnUs; }
DWORD XBEM_GUEST_ABI hle_GetSystemDefaultLCID() { return kLocaleEnUs; }
WORD XBEM_GUEST_ABI hle_GetUserDefaultUILanguage() { return kLocaleEnUs; }
WORD XBEM_GUEST_ABI hle_GetSystemDefaultUILanguage() { return kLocaleEnUs; }

INT XBEM_GUEST_ABI hle_GetUserDefaultLocaleName(LPWSTR buf, INT size) {
  static const std::u16string_view kName = u"en-US";
  if (!buf || size < static_cast<INT>(kName.size() + 1)) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  std::memcpy(buf, kName.data(), kName.size() * 2);
  buf[kName.size()] = 0;
  return static_cast<INT>(kName.size() + 1);
}

// ------------------------------------------------------- command line / env

LPSTR XBEM_GUEST_ABI hle_GetCommandLineA() { return const_cast<char*>(proc().command_line_a().c_str()); }
LPWSTR XBEM_GUEST_ABI hle_GetCommandLineW() { return const_cast<char16_t*>(proc().command_line_w().c_str()); }

std::mutex g_env_mutex;
std::map<std::string, std::u16string>& env_vars() {
  static std::map<std::string, std::u16string> vars;
  return vars;
}

std::string env_key(std::string name) {
  for (char& c : name)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return name;
}

template <class Char>
DWORD get_env(const std::string& name, Char* buf, DWORD size) {
  std::lock_guard lock(g_env_mutex);
  auto it = env_vars().find(env_key(name));
  if (it == env_vars().end()) {
    set_last_error(ERROR_ENVVAR_NOT_FOUND);
    return 0;
  }
  if constexpr (sizeof(Char) == 2) {
    return copy_out_string<char16_t>(it->second, buf, size);
  } else {
    const std::string v = utf16_to_utf8(it->second);
    return copy_out_string<char>(v, buf, size);
  }
}

DWORD XBEM_GUEST_ABI hle_GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size) { return get_env(narrow(name), buf, size); }
DWORD XBEM_GUEST_ABI hle_GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size) { return get_env(narrow(name), buf, size); }

BOOL XBEM_GUEST_ABI hle_SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
  if (!name) return FALSE;
  std::lock_guard lock(g_env_mutex);
  if (value) env_vars()[env_key(narrow(name))] = std::u16string(guest_wstr(value));
  else env_vars().erase(env_key(narrow(name)));
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
  if (!name) return FALSE;
  std::lock_guard lock(g_env_mutex);
  if (value) env_vars()[env_key(narrow(name))] = utf8_to_utf16(guest_cstr(value));
  else env_vars().erase(env_key(narrow(name)));
  return TRUE;
}

// The environment block is empty: a console title has no inherited environment worth emulating.
char16_t g_empty_env_w[2] = {0, 0};
char g_empty_env_a[2] = {0, 0};
LPWSTR XBEM_GUEST_ABI hle_GetEnvironmentStringsW() { return g_empty_env_w; }
LPSTR XBEM_GUEST_ABI hle_GetEnvironmentStrings() { return g_empty_env_a; }
BOOL XBEM_GUEST_ABI hle_FreeEnvironmentStringsW(LPWSTR) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_FreeEnvironmentStringsA(LPSTR) { return TRUE; }

void XBEM_GUEST_ABI hle_GetStartupInfoW(uint8_t* si) {
  if (!si) return;
  std::memset(si, 0, 104);  // sizeof(STARTUPINFOW) on x64
  const DWORD cb = 104;
  std::memcpy(si, &cb, 4);
}
void XBEM_GUEST_ABI hle_GetStartupInfoA(uint8_t* si) { hle_GetStartupInfoW(si); }

// ---------------------------------------------------------- system info

void fill_system_info(uint8_t* si) {
  std::memset(si, 0, 48);  // SYSTEM_INFO on x64
  const WORD arch = 9;    // PROCESSOR_ARCHITECTURE_AMD64
  const DWORD page = 4096;
  const uint64_t min_addr = 0x10000;
  const uint64_t max_addr = 0x7FFFFFFEFFFFull;
  const DWORD nproc = platform::hardware_threads();
  const uint64_t mask = nproc >= 64 ? ~0ull : ((1ull << nproc) - 1);
  const DWORD type = 8664;  // PROCESSOR_AMD_X8664
  const DWORD gran = 0x10000;
  const WORD level = 0x17;  // family 17h = Zen / Zen 2
  std::memcpy(si + 0, &arch, 2);
  std::memcpy(si + 4, &page, 4);
  std::memcpy(si + 8, &min_addr, 8);
  std::memcpy(si + 16, &max_addr, 8);
  std::memcpy(si + 24, &mask, 8);
  std::memcpy(si + 32, &nproc, 4);
  std::memcpy(si + 36, &type, 4);
  std::memcpy(si + 40, &gran, 4);
  std::memcpy(si + 44, &level, 2);
}

void XBEM_GUEST_ABI hle_GetSystemInfo(uint8_t* si) {
  if (si) fill_system_info(si);
}
void XBEM_GUEST_ABI hle_GetNativeSystemInfo(uint8_t* si) {
  if (si) fill_system_info(si);
}

BOOL XBEM_GUEST_ABI hle_GlobalMemoryStatusEx(uint8_t* ms) {
  if (!ms) return FALSE;
  // Memory available to games: Series X 13.5 GiB, Series S 8 GiB.
  const bool lockhart = proc().options().device == rt::DeviceType::XboxScarlettLockhart;
  const uint64_t total = lockhart ? (8ull << 30) : (27ull << 29);
  const uint64_t virt = 0x7FFFFFFE0000ull;
  const DWORD length = 64, load = 10;
  std::memcpy(ms + 0, &length, 4);
  std::memcpy(ms + 4, &load, 4);
  const uint64_t values[7] = {total, total - total / 10, total, total, virt, virt, 0};
  std::memcpy(ms + 8, values, sizeof values);
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_IsProcessorFeaturePresent(DWORD feature) {
  switch (feature) {
    case 6:   // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
    case 10:  // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
    case 12:  // PF_NX_ENABLED
    case 17:  // PF_RDTSC_INSTRUCTION_AVAILABLE
    case 23:  // PF_FASTFAIL_AVAILABLE
      return TRUE;
    case 13: return platform::host_has_feature("SSE3");
    case 36: return platform::host_has_feature("SSSE3");
    case 37: return platform::host_has_feature("SSE4.1");
    case 38: return platform::host_has_feature("SSE4.2");
    case 39: return platform::host_has_feature("AVX");
    case 40: return platform::host_has_feature("AVX2");
    case 41: return FALSE;  // AVX-512F: Zen 2 has none
    case 28: return platform::host_has_feature("RDRAND");
    default: return FALSE;
  }
}

// ------------------------------------------------------- pointers / SLists

void* XBEM_GUEST_ABI hle_EncodePointer(void* p) { return p; }
void* XBEM_GUEST_ABI hle_DecodePointer(void* p) { return p; }

// SLIST_HEADER is 16 bytes; only our functions touch it, so we use our own
// layout: [0] = first entry, [1] = depth. A mutex replaces cmpxchg16b.
std::mutex g_slist_mutex;

void XBEM_GUEST_ABI hle_InitializeSListHead(uint64_t* head) {
  head[0] = 0;
  head[1] = 0;
}

uint64_t XBEM_GUEST_ABI hle_InterlockedPushEntrySList(uint64_t* head, uint64_t* entry) {
  std::lock_guard lock(g_slist_mutex);
  const uint64_t old = head[0];
  entry[0] = old;
  head[0] = reinterpret_cast<uint64_t>(entry);
  ++head[1];
  return old;
}

uint64_t XBEM_GUEST_ABI hle_InterlockedPopEntrySList(uint64_t* head) {
  std::lock_guard lock(g_slist_mutex);
  const uint64_t first = head[0];
  if (first) {
    head[0] = *reinterpret_cast<uint64_t*>(first);
    --head[1];
  }
  return first;
}

uint64_t XBEM_GUEST_ABI hle_InterlockedFlushSList(uint64_t* head) {
  std::lock_guard lock(g_slist_mutex);
  const uint64_t first = head[0];
  head[0] = 0;
  head[1] = 0;
  return first;
}

WORD XBEM_GUEST_ABI hle_QueryDepthSList(uint64_t* head) { return static_cast<WORD>(head[1]); }

// --------------------------------------------------------------- exceptions

uint64_t g_unhandled_filter = 0;

uint64_t XBEM_GUEST_ABI hle_SetUnhandledExceptionFilter(uint64_t filter) {
  const uint64_t old = g_unhandled_filter;
  g_unhandled_filter = filter;
  return old;
}

LONG XBEM_GUEST_ABI hle_UnhandledExceptionFilter(void*) {
  XBEM_ERROR("kernelx", "guest reported an unhandled exception (UnhandledExceptionFilter)");
  return 1;  // EXCEPTION_EXECUTE_HANDLER: the CRT will terminate the process
}

constexpr DWORD kExceptionCppMsvc = 0xE06D7363;      // C++ throw
constexpr DWORD kExceptionSetThreadName = 0x406D1388;  // MSVC debugger thread-name convention
constexpr DWORD kExceptionDbgPrint = 0x40010006;     // DBG_PRINTEXCEPTION_C

void XBEM_GUEST_ABI hle_RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args) {
  if (code == kExceptionSetThreadName && nargs >= 3 && args) {
    // THREADNAME_INFO { DWORD type; LPCSTR name; DWORD thread_id; DWORD flags; }
    const auto* name = reinterpret_cast<const char*>(args[1]);
    if (name) XBEM_DEBUG("kernelx", "thread {} named \"{}\"", static_cast<int32_t>(args[2]), guest_cstr(name));
    return;  // a debugger-less __except handler would simply continue
  }
  if (code == kExceptionDbgPrint && nargs >= 2 && args) {
    log::guest_output(guest_cstr(reinterpret_cast<const char*>(args[1])));
    return;
  }
  // Structured exception handling (unwinding through .pdata/.xdata) is not
  // implemented yet. C++ exceptions therefore cannot be delivered.
  XBEM_ERROR("kernelx", "RaiseException(code {:#x}{}, flags {:#x}): SEH dispatch is not implemented yet", code,
             code == kExceptionCppMsvc ? " = C++ exception" : "", flags);
  proc().exit_process(code);
}

// RUNTIME_FUNCTION lookup in .pdata - needed by any SEH/unwind implementation.
void* XBEM_GUEST_ABI hle_RtlLookupFunctionEntry(uint64_t pc, uint64_t* image_base, void*) {
  LoadedModule* m = proc().loader().find_containing(pc);
  if (!m || !m->image) return nullptr;
  const auto& dir = m->image->directory(pe::kDirException);
  if (!dir.VirtualAddress || !dir.Size) return nullptr;
  const auto* table = reinterpret_cast<const pe::RuntimeFunction*>(m->base + dir.VirtualAddress);
  const auto rva = static_cast<uint32_t>(pc - m->handle());
  size_t lo = 0, hi = dir.Size / sizeof(pe::RuntimeFunction);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (rva < table[mid].BeginAddress) hi = mid;
    else if (rva >= table[mid].EndAddress) lo = mid + 1;
    else {
      if (image_base) *image_base = m->handle();
      return const_cast<pe::RuntimeFunction*>(&table[mid]);
    }
  }
  return nullptr;
}

}  // namespace

ModuleDef make_kernelx_module() {
  ModuleDef m;
  m.name = "kernelx.dll";
  // The console's kernel DLL. The PC names are aliases so the same code can
  // also serve PC GDK builds and test programs.
  m.aliases = {"kernel32.dll", "kernelbase.dll"};
  m.description = "Kernel/Win32 base API (threads, sync, memory, files, modules, time)";
  m.exports = {
      XBEM_HLE(OutputDebugStringA), XBEM_HLE(OutputDebugStringW), XBEM_HLE(IsDebuggerPresent), XBEM_HLE(DebugBreak),
      XBEM_HLE(GetTickCount), XBEM_HLE(GetTickCount64), XBEM_HLE(QueryPerformanceCounter),
      XBEM_HLE(QueryPerformanceFrequency), XBEM_HLE(QueryUnbiasedInterruptTime), XBEM_HLE(GetSystemTimeAsFileTime),
      XBEM_HLE(GetSystemTimePreciseAsFileTime), XBEM_HLE(GetSystemTime), XBEM_HLE(GetLocalTime), XBEM_HLE(Sleep),
      XBEM_HLE(SleepEx), XBEM_HLE(SwitchToThread), XBEM_HLE(GetLastError), XBEM_HLE(SetLastError),
      XBEM_HLE(GetCurrentProcess), XBEM_HLE(GetCurrentThread), XBEM_HLE(GetCurrentProcessId),
      XBEM_HLE(GetCurrentThreadId), XBEM_HLE(GetCurrentProcessorNumber), XBEM_HLE(ExitProcess),
      XBEM_HLE(TerminateProcess), XBEM_HLE(GetModuleHandleA), XBEM_HLE(GetModuleHandleW),
      XBEM_HLE(GetModuleHandleExA), XBEM_HLE(GetModuleHandleExW), XBEM_HLE(GetProcAddress), XBEM_HLE(LoadLibraryA),
      XBEM_HLE(LoadLibraryW), XBEM_HLE(LoadLibraryExA), XBEM_HLE(LoadLibraryExW), XBEM_HLE(FreeLibrary),
      XBEM_HLE(DisableThreadLibraryCalls), XBEM_HLE(GetModuleFileNameA), XBEM_HLE(GetModuleFileNameW),
      XBEM_HLE(MultiByteToWideChar), XBEM_HLE(WideCharToMultiByte), XBEM_HLE(lstrlenA), XBEM_HLE(lstrlenW),
      XBEM_HLE(GetACP), XBEM_HLE(GetOEMCP), XBEM_HLE(IsValidCodePage), XBEM_HLE(GetCPInfo),
      XBEM_HLE(GetUserDefaultLCID), XBEM_HLE(GetSystemDefaultLCID), XBEM_HLE(GetUserDefaultUILanguage),
      XBEM_HLE(GetSystemDefaultUILanguage), XBEM_HLE(GetUserDefaultLocaleName), XBEM_HLE(GetCommandLineA),
      XBEM_HLE(GetCommandLineW), XBEM_HLE(GetEnvironmentVariableA), XBEM_HLE(GetEnvironmentVariableW),
      XBEM_HLE(SetEnvironmentVariableA), XBEM_HLE(SetEnvironmentVariableW), XBEM_HLE(GetEnvironmentStringsW),
      XBEM_HLE(GetEnvironmentStrings), XBEM_HLE(FreeEnvironmentStringsW), XBEM_HLE(FreeEnvironmentStringsA),
      XBEM_HLE(GetStartupInfoW), XBEM_HLE(GetStartupInfoA), XBEM_HLE(GetSystemInfo), XBEM_HLE(GetNativeSystemInfo),
      XBEM_HLE(GlobalMemoryStatusEx), XBEM_HLE(IsProcessorFeaturePresent), XBEM_HLE(EncodePointer),
      XBEM_HLE(DecodePointer), XBEM_HLE(InitializeSListHead), XBEM_HLE(InterlockedPushEntrySList),
      XBEM_HLE(InterlockedPopEntrySList), XBEM_HLE(InterlockedFlushSList), XBEM_HLE(QueryDepthSList),
      XBEM_HLE(SetUnhandledExceptionFilter), XBEM_HLE(UnhandledExceptionFilter), XBEM_HLE(RaiseException),
      XBEM_HLE(RtlLookupFunctionEntry),
  };
  add_kernelx_memory_exports(m.exports);
  add_kernelx_sync_exports(m.exports);
  add_kernelx_file_exports(m.exports);
  return m;
}

}  // namespace xbem::hle
