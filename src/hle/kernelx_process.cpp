// kernelx: process/thread odds and ends, processor topology, time
// conversion, PE resources, app life-cycle notifications, console output and
// the COM helpers titles use for cross-thread marshaling.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <optional>
#include <thread>

#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "platform/thread.h"

namespace xbem::hle {
namespace {

using namespace win;
using rt::GuestThread;

constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002);
constexpr DWORD ERROR_RESOURCE_DATA_NOT_FOUND = 1812;
constexpr DWORD ERROR_RESOURCE_TYPE_NOT_FOUND = 1813;
constexpr DWORD ERROR_RESOURCE_NAME_NOT_FOUND = 1814;

template <class T>
T* guest_call_target(uint64_t address) {
  return reinterpret_cast<T*>(address);
}

// ------------------------------------------------------------------ time

constexpr uint64_t kTicksPerSecond = 10000000ull;
constexpr int64_t kEpochDays1601 = -134774;  // days from 1970-01-01 back to 1601-01-01

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yy + (m <= 2));
}

BOOL XBEM_GUEST_ABI hle_FileTimeToSystemTime(const uint64_t* ft, WORD* st) {
  if (!ft || !st || (*ft >> 63)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const uint64_t ticks = *ft;
  const int64_t days = static_cast<int64_t>(ticks / (86400 * kTicksPerSecond)) + kEpochDays1601;
  const uint64_t rem = ticks % (86400 * kTicksPerSecond);
  int y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  st[0] = static_cast<WORD>(y);
  st[1] = static_cast<WORD>(m);
  st[2] = static_cast<WORD>(((days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
  st[3] = static_cast<WORD>(d);
  st[4] = static_cast<WORD>(rem / (3600 * kTicksPerSecond));
  st[5] = static_cast<WORD>(rem / (60 * kTicksPerSecond) % 60);
  st[6] = static_cast<WORD>(rem / kTicksPerSecond % 60);
  st[7] = static_cast<WORD>(rem / 10000 % 1000);
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SystemTimeToFileTime(const WORD* st, uint64_t* ft) {
  if (!st || !ft || st[1] < 1 || st[1] > 12 || st[3] < 1 || st[3] > 31 || st[4] > 23 || st[5] > 59 || st[6] > 59 ||
      st[7] > 999 || st[0] < 1601) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const int64_t days = days_from_civil(st[0], st[1], st[3]) - kEpochDays1601;
  const uint64_t secs = static_cast<uint64_t>(days) * 86400 + st[4] * 3600ull + st[5] * 60ull + st[6];
  *ft = secs * kTicksPerSecond + st[7] * 10000ull;
  return TRUE;
}

// Offset of local time from UTC in seconds (host time zone).
int64_t local_offset_seconds() {
  const std::time_t now = std::time(nullptr);
#if defined(_WIN32)
  long bias = 0;
  _get_timezone(&bias);
  return -bias;
#else
  std::tm tm{};
  localtime_r(&now, &tm);
  return tm.tm_gmtoff;
#endif
}

BOOL XBEM_GUEST_ABI hle_FileTimeToLocalFileTime(const uint64_t* in, uint64_t* out) {
  if (!in || !out) return FALSE;
  *out = *in + static_cast<uint64_t>(local_offset_seconds() * static_cast<int64_t>(kTicksPerSecond));
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_LocalFileTimeToFileTime(const uint64_t* in, uint64_t* out) {
  if (!in || !out) return FALSE;
  *out = *in - static_cast<uint64_t>(local_offset_seconds() * static_cast<int64_t>(kTicksPerSecond));
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_GetSystemTimeAdjustment(DWORD* adjustment, DWORD* increment, BOOL* disabled) {
  if (adjustment) *adjustment = 156250;
  if (increment) *increment = 156250;
  if (disabled) *disabled = TRUE;
  return TRUE;
}

// TIME_ZONE_INFORMATION (172 bytes): only the bias is meaningful here.
DWORD XBEM_GUEST_ABI hle_GetTimeZoneInformation(uint8_t* tzi) {
  if (!tzi) return 0xFFFFFFFF;
  std::memset(tzi, 0, 172);
  const LONG bias = static_cast<LONG>(-local_offset_seconds() / 60);
  std::memcpy(tzi, &bias, 4);
  const char16_t name[] = u"Coordinated Universal Time";
  std::memcpy(tzi + 4, name, sizeof name);
  std::memcpy(tzi + 88, name, sizeof name);
  return 0;  // TIME_ZONE_ID_UNKNOWN (no daylight rules)
}

// ------------------------------------------------- processor topology

// Console-like topology from the host thread count: SMT pairs, two 4-core
// CCXs sharing an L3 each (Zen 2).
struct Topology {
  unsigned logical;
  unsigned cores;
  unsigned threads_per_core;
};

Topology topology() {
  const unsigned n = std::max(1u, platform::hardware_threads());
  const unsigned per_core = (n >= 2 && n % 2 == 0) ? 2 : 1;
  return {n, n / per_core, per_core};
}

uint64_t core_mask(const Topology& t, unsigned core) {
  const uint64_t bits = (1ull << t.threads_per_core) - 1;
  const unsigned shift = core * t.threads_per_core;
  return shift >= 64 ? 0 : bits << shift;
}

uint64_t ccx_mask(const Topology& t, unsigned ccx) {
  uint64_t m = 0;
  for (unsigned c = ccx * 4; c < std::min(t.cores, ccx * 4 + 4); ++c) m |= core_mask(t, c);
  return m;
}

uint64_t all_mask(const Topology& t) { return t.logical >= 64 ? ~0ull : (1ull << t.logical) - 1; }

struct CacheInfo {
  uint8_t level, associativity;
  uint16_t line;
  uint32_t size;
  uint32_t type;  // 0 unified, 1 instruction, 2 data
};
constexpr CacheInfo kCaches[] = {{1, 8, 64, 32 << 10, 2}, {1, 8, 64, 32 << 10, 1}, {2, 8, 64, 512 << 10, 0}};
constexpr CacheInfo kL3 = {3, 16, 64, 4 << 20, 0};

// SYSTEM_LOGICAL_PROCESSOR_INFORMATION (32 bytes each).
BOOL XBEM_GUEST_ABI hle_GetLogicalProcessorInformation(uint8_t* buffer, DWORD* length) {
  if (!length) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const Topology t = topology();
  std::vector<std::array<uint8_t, 32>> out;
  auto add = [&](uint64_t mask, DWORD relation, auto fill) {
    std::array<uint8_t, 32> e{};
    std::memcpy(e.data(), &mask, 8);
    std::memcpy(e.data() + 8, &relation, 4);
    fill(e.data() + 16);
    out.push_back(e);
  };
  for (unsigned c = 0; c < t.cores; ++c) {
    add(core_mask(t, c), 0, [&](uint8_t* u) { u[0] = t.threads_per_core > 1 ? 1 : 0; });  // ProcessorCore: Flags
    for (const CacheInfo& ci : kCaches)
      add(core_mask(t, c), 2, [&](uint8_t* u) { std::memcpy(u, &ci, 12); });
  }
  for (unsigned x = 0; x * 4 < t.cores; ++x) add(ccx_mask(t, x), 2, [&](uint8_t* u) { std::memcpy(u, &kL3, 12); });
  add(all_mask(t), 3, [](uint8_t*) {});  // ProcessorPackage
  add(all_mask(t), 1, [](uint8_t*) {});  // NumaNode 0
  const DWORD need = static_cast<DWORD>(out.size() * 32);
  if (!buffer || *length < need) {
    *length = need;
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }
  for (size_t i = 0; i < out.size(); ++i) std::memcpy(buffer + i * 32, out[i].data(), 32);
  *length = need;
  return TRUE;
}

// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX (variable size records).
BOOL XBEM_GUEST_ABI hle_GetLogicalProcessorInformationEx(DWORD relation, uint8_t* buffer, DWORD* length) {
  if (!length) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  constexpr DWORD kAll = 0xFFFF;
  const Topology t = topology();
  std::vector<uint8_t> out;
  auto record = [&](DWORD rel, DWORD size) -> uint8_t* {
    const size_t at = out.size();
    out.resize(at + size, 0);
    std::memcpy(out.data() + at, &rel, 4);
    std::memcpy(out.data() + at + 4, &size, 4);
    return out.data() + at + 8;
  };
  auto group_affinity = [](uint8_t* p, uint64_t mask) { std::memcpy(p, &mask, 8); };  // Group 0
  auto want = [&](DWORD r) { return relation == kAll || relation == r; };

  if (want(0))  // RelationProcessorCore: PROCESSOR_RELATIONSHIP
    for (unsigned c = 0; c < t.cores; ++c) {
      uint8_t* p = record(0, 48);
      p[0] = t.threads_per_core > 1 ? 1 : 0;  // LTP_PC_SMT
      const WORD groups = 1;
      std::memcpy(p + 22, &groups, 2);
      group_affinity(p + 24, core_mask(t, c));
    }
  if (want(1)) {  // RelationNumaNode: NUMA_NODE_RELATIONSHIP
    uint8_t* p = record(1, 48);
    const WORD groups = 1;
    std::memcpy(p + 22, &groups, 2);
    group_affinity(p + 24, all_mask(t));
  }
  if (want(2)) {  // RelationCache: CACHE_RELATIONSHIP
    auto cache = [&](const CacheInfo& ci, uint64_t mask) {
      uint8_t* p = record(2, 56);
      std::memcpy(p, &ci, 12);
      const WORD groups = 1;
      std::memcpy(p + 30, &groups, 2);
      group_affinity(p + 32, mask);
    };
    for (unsigned c = 0; c < t.cores; ++c)
      for (const CacheInfo& ci : kCaches) cache(ci, core_mask(t, c));
    for (unsigned x = 0; x * 4 < t.cores; ++x) cache(kL3, ccx_mask(t, x));
  }
  if (want(3)) {  // RelationProcessorPackage
    uint8_t* p = record(3, 48);
    const WORD groups = 1;
    std::memcpy(p + 22, &groups, 2);
    group_affinity(p + 24, all_mask(t));
  }
  if (want(4)) {  // RelationGroup: GROUP_RELATIONSHIP with one PROCESSOR_GROUP_INFO
    uint8_t* p = record(4, 80);
    const WORD one = 1;
    std::memcpy(p, &one, 2);
    std::memcpy(p + 2, &one, 2);
    p[24] = static_cast<uint8_t>(std::min(t.logical, 64u));
    p[25] = p[24];
    const uint64_t mask = all_mask(t);
    std::memcpy(p + 24 + 40, &mask, 8);
  }
  if (out.empty()) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const DWORD need = static_cast<DWORD>(out.size());
  if (!buffer || *length < need) {
    *length = need;
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }
  std::memcpy(buffer, out.data(), need);
  *length = need;
  return TRUE;
}

// --------------------------------------------------- processes / threads

using CreateThreadFn = HANDLE(XBEM_GUEST_ABI*)(void*, SIZE_T, uint64_t, void*, DWORD, DWORD*);

HANDLE XBEM_GUEST_ABI hle_CreateRemoteThreadEx(HANDLE process, void* sa, SIZE_T stack, uint64_t start, void* param,
                                               DWORD flags, void*, DWORD* tid) {
  if (process != kCurrentProcessPseudo && process != proc().process_id()) {
    XBEM_WARN_ONCE("kernelx", "CreateRemoteThreadEx into another process is not supported");
  }
  auto look = Registry::instance().find("kernelx.dll", "CreateThread");
  if (!look.exp) return 0;
  return reinterpret_cast<CreateThreadFn>(look.exp->address)(sa, stack, start, param, flags, tid);
}

// PROC_THREAD_ATTRIBUTE_LIST is opaque; callers size it with a first call.
BOOL XBEM_GUEST_ABI hle_InitializeProcThreadAttributeList(uint8_t* list, DWORD count, DWORD, SIZE_T* size) {
  const SIZE_T need = 48 + static_cast<SIZE_T>(count) * 24;
  if (!size) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (!list || *size < need) {
    *size = need;
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }
  std::memset(list, 0, need);
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_UpdateProcThreadAttribute(void*, DWORD, uint64_t, void*, SIZE_T, void*, SIZE_T*) {
  return TRUE;  // ideal processor / group affinity hints are accepted and ignored
}
void XBEM_GUEST_ABI hle_DeleteProcThreadAttributeList(void*) {}

void XBEM_GUEST_ABI hle_FlushProcessWriteBuffers() { platform::flush_process_write_buffers(); }

DWORD XBEM_GUEST_ABI hle_GetProcessId(HANDLE process) {
  if (process == kCurrentProcessPseudo) return proc().process_id();
  set_last_error(ERROR_INVALID_HANDLE);
  return 0;
}

HANDLE XBEM_GUEST_ABI hle_OpenThread(DWORD, BOOL, DWORD id) {
  auto t = proc().find_thread(id);
  if (!t) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return proc().handles().insert(t);
}

void XBEM_GUEST_ABI hle_GetCurrentThreadStackLimits(uint64_t* low, uint64_t* high) {
  uint64_t lo = 0, hi = 0;
  if (GuestThread* t = rt::current_thread(); t && t->teb) {
    std::memcpy(&lo, t->teb + rt::teb::kStackLimit, 8);
    std::memcpy(&hi, t->teb + rt::teb::kStackBase, 8);
  }
  if (low) *low = lo;
  if (high) *high = hi;
}

std::shared_ptr<GuestThread> thread_from(HANDLE h) { return std::dynamic_pointer_cast<GuestThread>(lookup_handle(h)); }

// PROCESSOR_NUMBER { WORD Group; BYTE Number; BYTE Reserved; }
BOOL XBEM_GUEST_ABI hle_SetThreadIdealProcessorEx(HANDLE h, const uint8_t*, uint8_t* previous) {
  if (!thread_from(h)) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (previous) std::memset(previous, 0, 4);
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_GetThreadIdealProcessorEx(HANDLE h, uint8_t* ideal) {
  if (!thread_from(h) || !ideal) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  std::memset(ideal, 0, 4);
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SetProcessPriorityBoost(HANDLE, BOOL) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_GetProcessPriorityBoost(HANDLE, BOOL* disabled) {
  if (disabled) *disabled = FALSE;
  return TRUE;
}

// GROUP_AFFINITY { KAFFINITY Mask; WORD Group; WORD Reserved[3]; }
BOOL XBEM_GUEST_ABI hle_GetThreadGroupAffinity(HANDLE h, uint8_t* affinity) {
  auto t = thread_from(h);
  if (!t || !affinity) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  std::memset(affinity, 0, 16);
  const uint64_t mask = t->affinity_mask & all_mask(topology());
  std::memcpy(affinity, &mask, 8);
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_SetThreadGroupAffinity(HANDLE h, const uint8_t* affinity, uint8_t* previous) {
  auto t = thread_from(h);
  if (!t || !affinity) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (previous) {
    std::memset(previous, 0, 16);
    const uint64_t old = t->affinity_mask & all_mask(topology());
    std::memcpy(previous, &old, 8);
  }
  std::memcpy(&t->affinity_mask, affinity, 8);  // recorded; host scheduling is left to the OS
  return TRUE;
}

SIZE_T XBEM_GUEST_ABI hle_VirtualQueryEx(HANDLE process, const void* address, uint8_t* mbi, SIZE_T length) {
  if (process != kCurrentProcessPseudo && process != proc().process_id()) {
    set_last_error(ERROR_ACCESS_DENIED);
    return 0;
  }
  return virtual_query(address, mbi, length);
}

// void RaiseFailFastException(PEXCEPTION_RECORD, PCONTEXT, DWORD flags)
[[noreturn]] void XBEM_GUEST_ABI hle_RaiseFailFastException(const uint8_t* record, const uint8_t*, DWORD flags) {
  DWORD code = 0xC0000602;  // STATUS_FAIL_FAST_EXCEPTION
  uint64_t address = 0;
  if (record) {
    std::memcpy(&code, record, 4);
    std::memcpy(&address, record + 16, 8);
  }
  XBEM_ERROR("kernelx", "RaiseFailFastException: code {:#010x} at {} (flags {:#x}, called from {}) - terminating",
             code, describe_code_address(address), flags, describe_code_address(XBEM_RETURN_ADDRESS()));
  proc().exit_process(code);
}

// ------------------------------------------------ error reporting / PLM

HRESULT XBEM_GUEST_ABI hle_WerRegisterFile(LPCWSTR path, DWORD, DWORD) {
  XBEM_DEBUG("kernelx", "WerRegisterFile({})", narrow(path));
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_WerUnregisterFile(LPCWSTR) { return S_OK; }

// Process lifetime management: the title is never suspended or constrained
// here, so the callbacks are recorded but never invoked.
std::atomic<uint64_t> g_next_registration{0x1000};

DWORD XBEM_GUEST_ABI hle_RegisterAppStateChangeNotification(uint64_t callback, void*, uint64_t* registration) {
  if (!callback || !registration) return ERROR_INVALID_PARAMETER;
  *registration = g_next_registration.fetch_add(0x10);
  return ERROR_SUCCESS;
}
void XBEM_GUEST_ABI hle_UnregisterAppStateChangeNotification(uint64_t) {}
DWORD XBEM_GUEST_ABI hle_RegisterAppConstrainedChangeNotification(uint64_t callback, void*, uint64_t* registration) {
  return hle_RegisterAppStateChangeNotification(callback, nullptr, registration);
}
void XBEM_GUEST_ABI hle_UnregisterAppConstrainedChangeNotification(uint64_t) {}

// ----------------------------------------------------------------- COM

// A process-lifetime object that answers every QueryInterface with
// E_NOINTERFACE; used where COM hands out identity tokens.
HRESULT XBEM_GUEST_ABI null_query_interface(void*, const void*, void** out) {
  if (out) *out = nullptr;
  return E_NOINTERFACE;
}
ULONG XBEM_GUEST_ABI null_add_ref(void*) { return 2; }
ULONG XBEM_GUEST_ABI null_release(void*) { return 1; }
HRESULT XBEM_GUEST_ABI null_not_impl() { return E_NOTIMPL; }

void* const g_null_vtable[16] = {
    reinterpret_cast<void*>(&null_query_interface), reinterpret_cast<void*>(&null_add_ref),
    reinterpret_cast<void*>(&null_release),         reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),        reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),        reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),        reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),        reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),        reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),        reinterpret_cast<void*>(&null_not_impl),
};
struct {
  void* const* vtable = g_null_vtable;
} g_context_object;

HRESULT XBEM_GUEST_ABI hle_CoGetContextToken(uint64_t* token) {
  if (!token) return E_POINTER;
  *token = reinterpret_cast<uint64_t>(&g_context_object);
  return S_OK;
}

std::atomic<ULONG> g_server_refs{0};
ULONG XBEM_GUEST_ABI hle_CoAddRefServerProcess() { return ++g_server_refs; }
ULONG XBEM_GUEST_ABI hle_CoReleaseServerProcess() { return g_server_refs ? --g_server_refs : 0; }

// Inter-thread marshaling: every thread is in the multithreaded apartment,
// so a "stream" simply carries a reference to the object.
constexpr uint64_t kStreamMagic = 0x4D52545342454258ull;  // "XBEBSTRM"

struct MarshalStream {
  void* const* vtable;
  uint64_t magic;
  void* object;
  std::atomic<ULONG> refs;
};

using QiFn = HRESULT(XBEM_GUEST_ABI*)(void*, const void*, void**);
using RefFn = ULONG(XBEM_GUEST_ABI*)(void*);

void* const* vtable_of(void* object) { return *static_cast<void* const* const*>(object); }

HRESULT XBEM_GUEST_ABI stream_query_interface(MarshalStream* s, const void*, void** out) {
  // IUnknown / IStream identity is all a caller can legitimately ask for.
  if (!out) return E_POINTER;
  s->refs.fetch_add(1);
  *out = s;
  return S_OK;
}
ULONG XBEM_GUEST_ABI stream_add_ref(MarshalStream* s) { return s->refs.fetch_add(1) + 1; }
ULONG XBEM_GUEST_ABI stream_release(MarshalStream* s) {
  const ULONG n = s->refs.fetch_sub(1) - 1;
  if (n == 0) {
    if (s->object) reinterpret_cast<RefFn>(vtable_of(s->object)[2])(s->object);
    s->magic = 0;
    delete s;
  }
  return n;
}

void* const g_stream_vtable[14] = {
    reinterpret_cast<void*>(&stream_query_interface), reinterpret_cast<void*>(&stream_add_ref),
    reinterpret_cast<void*>(&stream_release),         reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),          reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),          reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),          reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),          reinterpret_cast<void*>(&null_not_impl),
    reinterpret_cast<void*>(&null_not_impl),          reinterpret_cast<void*>(&null_not_impl),
};

HRESULT XBEM_GUEST_ABI hle_CoMarshalInterThreadInterfaceInStream(const void*, void* object, void** stream) {
  if (!object || !stream) return E_INVALIDARG;
  reinterpret_cast<RefFn>(vtable_of(object)[1])(object);  // AddRef
  auto* s = new MarshalStream{g_stream_vtable, kStreamMagic, object, {1}};
  *stream = s;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_CoGetInterfaceAndReleaseStream(void* stream, const void* iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  auto* s = static_cast<MarshalStream*>(stream);
  if (!s || s->magic != kStreamMagic) return E_INVALIDARG;
  const HRESULT hr = reinterpret_cast<QiFn>(vtable_of(s->object)[0])(s->object, iid, out);
  stream_release(s);
  return hr;
}

// ------------------------------------------------------------- console

BOOL XBEM_GUEST_ABI hle_WriteConsoleW(HANDLE, const char16_t* text, DWORD count, DWORD* written, void*) {
  if (!text) return FALSE;
  const std::string s = utf16_to_utf8(std::u16string_view(text, count));
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
  if (written) *written = count;
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_WriteConsoleA(HANDLE, const char* text, DWORD count, DWORD* written, void*) {
  if (!text) return FALSE;
  std::fwrite(text, 1, count, stdout);
  std::fflush(stdout);
  if (written) *written = count;
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_GetConsoleScreenBufferInfo(HANDLE, void*) {
  set_last_error(ERROR_INVALID_HANDLE);  // titles have no console window
  return FALSE;
}
BOOL XBEM_GUEST_ABI hle_GetConsoleMode(HANDLE, DWORD*) {
  set_last_error(ERROR_INVALID_HANDLE);
  return FALSE;
}
BOOL XBEM_GUEST_ABI hle_SetConsoleMode(HANDLE, DWORD) {
  set_last_error(ERROR_INVALID_HANDLE);
  return FALSE;
}
uint64_t XBEM_GUEST_ABI hle_GetConsoleWindow() { return 0; }

// --------------------------------------------------------------- WinRT

HRESULT XBEM_GUEST_ABI hle_RoRegisterActivationFactories(void*, void*, DWORD count, uint64_t* cookie) {
  XBEM_DEBUG("kernelx", "RoRegisterActivationFactories({} classes)", count);
  if (cookie) *cookie = g_next_registration.fetch_add(0x10);
  return S_OK;
}
void XBEM_GUEST_ABI hle_RoRevokeActivationFactories(uint64_t) {}

// ----------------------------------------------------------- resources

uint64_t module_base(HMODULE h) {
  if (!h) {
    uint64_t base = 0;
    std::memcpy(&base, proc().peb() + rt::peb::kImageBase, 8);
    return base;
  }
  rt::LoadedModule* m = proc().loader().find_by_handle(h & ~3ull);  // LOAD_LIBRARY_AS_DATAFILE tags
  return (m && !m->hle) ? m->handle() : 0;
}

struct ResourceSection {
  const uint8_t* base = nullptr;  // image base
  const uint8_t* root = nullptr;  // resource directory
  uint32_t size = 0;
};

std::optional<ResourceSection> resources_of(uint64_t base) {
  if (!base) return std::nullopt;
  const auto* img = reinterpret_cast<const uint8_t*>(base);
  uint32_t lfanew;
  std::memcpy(&lfanew, img + 0x3C, 4);
  const uint8_t* nt = img + lfanew;
  uint32_t rva, size;
  std::memcpy(&rva, nt + 24 + 112 + 2 * 8, 4);  // OptionalHeader64.DataDirectory[RESOURCE]
  std::memcpy(&size, nt + 24 + 112 + 2 * 8 + 4, 4);
  if (!rva || !size) return std::nullopt;
  return ResourceSection{img, img + rva, size};
}

struct ResourceKey {
  bool is_id = true;
  uint16_t id = 0;
  std::u16string name;
};

ResourceKey key_of(LPCWSTR v) {
  ResourceKey k;
  const auto raw = reinterpret_cast<uint64_t>(v);
  if (raw < 0x10000) {
    k.id = static_cast<uint16_t>(raw);
    return k;
  }
  std::u16string_view s = guest_wstr(v);
  if (!s.empty() && s[0] == u'#') {
    unsigned n = 0;
    for (char16_t c : s.substr(1)) n = n * 10 + (c - u'0');
    k.id = static_cast<uint16_t>(n);
    return k;
  }
  k.is_id = false;
  for (char16_t c : s) k.name.push_back(c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - 32) : c);
  return k;
}

// Returns the OffsetToData of the matching entry in a directory (or of the
// first entry when `first` is set).
std::optional<uint32_t> find_entry(const ResourceSection& rs, uint32_t dir_offset, const ResourceKey* key) {
  if (dir_offset + 16 > rs.size) return std::nullopt;
  const uint8_t* dir = rs.root + dir_offset;
  uint16_t named, ids;
  std::memcpy(&named, dir + 12, 2);
  std::memcpy(&ids, dir + 14, 2);
  const uint32_t count = named + ids;
  if (dir_offset + 16 + count * 8ull > rs.size) return std::nullopt;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t name, data;
    std::memcpy(&name, dir + 16 + i * 8, 4);
    std::memcpy(&data, dir + 16 + i * 8 + 4, 4);
    if (!key) return data;
    if (key->is_id) {
      if (!(name & 0x80000000u) && (name & 0xFFFF) == key->id) return data;
    } else if (name & 0x80000000u) {
      const uint32_t off = name & 0x7FFFFFFF;
      if (off + 2 > rs.size) continue;
      uint16_t len;
      std::memcpy(&len, rs.root + off, 2);
      if (len != key->name.size() || off + 2 + len * 2ull > rs.size) continue;
      bool equal = true;
      for (uint16_t j = 0; j < len && equal; ++j) {
        char16_t c;
        std::memcpy(&c, rs.root + off + 2 + j * 2, 2);
        if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
        equal = c == key->name[j];
      }
      if (equal) return data;
    }
  }
  return std::nullopt;
}

uint64_t find_resource(HMODULE module, LPCWSTR type, LPCWSTR name, WORD language) {
  auto rs = resources_of(module_base(module));
  if (!rs) {
    set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return 0;
  }
  const ResourceKey type_key = key_of(type), name_key = key_of(name);
  auto t = find_entry(*rs, 0, &type_key);
  if (!t || !(*t & 0x80000000u)) {
    set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return 0;
  }
  auto n = find_entry(*rs, *t & 0x7FFFFFFF, &name_key);
  if (!n || !(*n & 0x80000000u)) {
    set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
    return 0;
  }
  std::optional<uint32_t> l;
  if (language) {
    ResourceKey lang_key;
    lang_key.id = language;
    l = find_entry(*rs, *n & 0x7FFFFFFF, &lang_key);
  }
  if (!l) l = find_entry(*rs, *n & 0x7FFFFFFF, nullptr);
  if (!l || (*l & 0x80000000u) || *l + 16 > rs->size) {
    set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return 0;
  }
  return reinterpret_cast<uint64_t>(rs->root + *l);  // HRSRC = IMAGE_RESOURCE_DATA_ENTRY*
}

uint64_t XBEM_GUEST_ABI hle_FindResourceExW(HMODULE module, LPCWSTR type, LPCWSTR name, WORD language) {
  return find_resource(module, type, name, language);
}
uint64_t XBEM_GUEST_ABI hle_FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type) {
  return find_resource(module, type, name, 0);
}

uint64_t resource_image_base(uint64_t hrsrc) {
  rt::LoadedModule* m = proc().loader().find_containing(hrsrc);
  return m ? m->handle() : 0;
}

uint64_t XBEM_GUEST_ABI hle_LoadResource(HMODULE, uint64_t hrsrc) {
  const uint64_t base = hrsrc ? resource_image_base(hrsrc) : 0;
  if (!base) {
    set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return 0;
  }
  uint32_t rva;
  std::memcpy(&rva, reinterpret_cast<const void*>(hrsrc), 4);
  return base + rva;
}
uint64_t XBEM_GUEST_ABI hle_LockResource(uint64_t data) { return data; }
BOOL XBEM_GUEST_ABI hle_FreeResource(uint64_t) { return FALSE; }
DWORD XBEM_GUEST_ABI hle_SizeofResource(HMODULE, uint64_t hrsrc) {
  if (!hrsrc) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  DWORD size;
  std::memcpy(&size, reinterpret_cast<const uint8_t*>(hrsrc) + 4, 4);
  return size;
}

}  // namespace

void add_kernelx_process_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(FileTimeToSystemTime), XBEM_HLE(SystemTimeToFileTime), XBEM_HLE(FileTimeToLocalFileTime),
      XBEM_HLE(LocalFileTimeToFileTime), XBEM_HLE(GetSystemTimeAdjustment), XBEM_HLE(GetTimeZoneInformation),
      XBEM_HLE(GetLogicalProcessorInformation), XBEM_HLE(GetLogicalProcessorInformationEx),
      XBEM_HLE(CreateRemoteThreadEx), XBEM_HLE(InitializeProcThreadAttributeList),
      XBEM_HLE(UpdateProcThreadAttribute), XBEM_HLE(DeleteProcThreadAttributeList),
      XBEM_HLE(FlushProcessWriteBuffers), XBEM_HLE(GetProcessId), XBEM_HLE(OpenThread),
      XBEM_HLE(GetCurrentThreadStackLimits), XBEM_HLE(SetThreadIdealProcessorEx),
      XBEM_HLE(GetThreadIdealProcessorEx), XBEM_HLE(SetProcessPriorityBoost), XBEM_HLE(GetProcessPriorityBoost),
      XBEM_HLE(GetThreadGroupAffinity), XBEM_HLE(SetThreadGroupAffinity), XBEM_HLE(VirtualQueryEx),
      XBEM_HLE(RaiseFailFastException), XBEM_HLE(WerRegisterFile), XBEM_HLE(WerUnregisterFile),
      XBEM_HLE(RegisterAppStateChangeNotification), XBEM_HLE(UnregisterAppStateChangeNotification),
      XBEM_HLE(RegisterAppConstrainedChangeNotification), XBEM_HLE(UnregisterAppConstrainedChangeNotification),
      XBEM_HLE(CoGetContextToken), XBEM_HLE(CoAddRefServerProcess), XBEM_HLE(CoReleaseServerProcess),
      XBEM_HLE(CoMarshalInterThreadInterfaceInStream), XBEM_HLE(CoGetInterfaceAndReleaseStream),
      XBEM_HLE(WriteConsoleW), XBEM_HLE(WriteConsoleA), XBEM_HLE_STUB(GetConsoleScreenBufferInfo),
      XBEM_HLE_STUB(GetConsoleMode), XBEM_HLE_STUB(SetConsoleMode), XBEM_HLE(GetConsoleWindow),
      XBEM_HLE(RoRegisterActivationFactories), XBEM_HLE(RoRevokeActivationFactories),
      XBEM_HLE(FindResourceExW), XBEM_HLE(FindResourceW), XBEM_HLE(LoadResource), XBEM_HLE(LockResource),
      XBEM_HLE(FreeResource), XBEM_HLE(SizeofResource),
  });
}

}  // namespace xbem::hle
