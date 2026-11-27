// kernelx: threads, waits, kernel synchronisation objects, user-mode locks
// (critical sections, SRW locks, condition variables, init-once,
// WaitOnAddress) and TLS/FLS slots.

#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <csetjmp>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "platform/thread.h"

namespace xbem::hle {

void exit_guest_thread(uint32_t code) {
  rt::GuestThread* t = rt::current_thread();
  if (!t || !t->exit_jump_valid) {
    XBEM_ERROR("kernelx", "ExitThread called outside a guest thread");
    proc().exit_process(code);
  }
  {
    std::lock_guard lock(rt::sync_mutex());
    t->exit_code = code;
  }
  std::longjmp(t->exit_jump, 1);  // unwinds to the thread's start frame in Process::thread_main
}

// XBEM_AFFINITY=1: guest affinity masks restrict the host threads (threads a
// title pins to one core then take turns there instead of running at once).
// The first calls are logged either way.
void apply_thread_affinity(rt::GuestThread& t, const char* how) {
  static const bool apply = [] {
    const char* v = std::getenv("XBEM_AFFINITY");
    return v && *v == '1';
  }();
  static std::atomic<int> logged{0};
  const bool ok = apply && !t.finished && platform::set_thread_affinity(t.native, t.affinity_mask);
  if (logged.fetch_add(1) < 128)
    XBEM_INFO("kernelx", "{}: guest thread {} (entry {}) affinity {:#x}{}", how, t.id,
              describe_code_address(t.start_routine), t.affinity_mask,
              apply ? (ok ? " - applied to the host thread" : " - NOT applied") : "");
}

namespace {

using namespace win;
using rt::GuestThread;
using rt::KernelObject;

// ------------------------------------------------------------------ threads

constexpr DWORD CREATE_SUSPENDED = 0x4;

HANDLE XBEM_GUEST_ABI hle_CreateThread(void*, SIZE_T stack_size, uint64_t start, void* param, DWORD flags,
                                       DWORD* thread_id) {
  if (!start) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  auto t = proc().create_thread(start, reinterpret_cast<uint64_t>(param), stack_size, flags & CREATE_SUSPENDED);
  if (!t) {
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  if (thread_id) *thread_id = t->id;
  const HANDLE h = proc().handles().insert(t);
  XBEM_INFO("kernelx", "CreateThread: guest thread {} at {} (param {}) -> handle {:#x}, from {}", t->id,
            describe_code_address(start), param, h, describe_code_address(XBEM_RETURN_ADDRESS()));
  return h;
}

[[noreturn]] void XBEM_GUEST_ABI hle_ExitThread(DWORD code) { exit_guest_thread(code); }

BOOL XBEM_GUEST_ABI hle_GetExitCodeThread(HANDLE h, DWORD* code) {
  auto t = std::dynamic_pointer_cast<GuestThread>(lookup_handle(h));
  if (!t || !code) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  std::lock_guard lock(rt::sync_mutex());
  *code = t->finished ? t->exit_code : STILL_ACTIVE;
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_GetThreadId(HANDLE h) {
  auto t = std::dynamic_pointer_cast<GuestThread>(lookup_handle(h));
  return t ? t->id : 0;
}

DWORD XBEM_GUEST_ABI hle_ResumeThread(HANDLE h) {
  auto t = std::dynamic_pointer_cast<GuestThread>(lookup_handle(h));
  if (!t) {
    set_last_error(ERROR_INVALID_HANDLE);
    return static_cast<DWORD>(-1);
  }
  return static_cast<DWORD>(proc().resume_thread(*t));
}

DWORD XBEM_GUEST_ABI hle_SuspendThread(HANDLE) {
  XBEM_WARN_ONCE("kernelx", "SuspendThread is not supported (host threads cannot be frozen safely)");
  set_last_error(ERROR_NOT_SUPPORTED);
  return static_cast<DWORD>(-1);
}

BOOL XBEM_GUEST_ABI hle_SetThreadPriority(HANDLE h, INT priority) {
  if (auto t = std::dynamic_pointer_cast<GuestThread>(lookup_handle(h))) t->priority = priority;
  return TRUE;
}

INT XBEM_GUEST_ABI hle_GetThreadPriority(HANDLE h) {
  auto t = std::dynamic_pointer_cast<GuestThread>(lookup_handle(h));
  return t ? t->priority : 0;
}

// Console titles pin threads to cores explicitly. We record the mask so the
// round-trip is correct, but leave scheduling to the host OS.
DWORD_PTR XBEM_GUEST_ABI hle_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
  auto t = std::dynamic_pointer_cast<GuestThread>(lookup_handle(h));
  if (!t || mask == 0) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  const DWORD_PTR old = t->affinity_mask;
  t->affinity_mask = mask;
  apply_thread_affinity(*t, "SetThreadAffinityMask");
  return old;
}

DWORD XBEM_GUEST_ABI hle_SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }

HRESULT XBEM_GUEST_ABI hle_SetThreadDescription(HANDLE h, LPCWSTR desc) {
  auto t = std::dynamic_pointer_cast<GuestThread>(lookup_handle(h));
  if (t && desc) {
    XBEM_DEBUG("kernelx", "thread {} description \"{}\"", t->id, narrow(desc));
    if (t.get() == rt::current_thread()) platform::set_current_thread_name(narrow(desc));
  }
  return S_OK;
}

// ------------------------------------------------------------- handles/waits

BOOL XBEM_GUEST_ABI hle_CloseHandle(HANDLE h) {
  if (h == kCurrentProcessPseudo || h == kCurrentThreadPseudo) return TRUE;
  if (!proc().handles().close(h)) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_DuplicateHandle(HANDLE, HANDLE source, HANDLE, HANDLE* target, DWORD, BOOL, DWORD options) {
  constexpr DWORD DUPLICATE_CLOSE_SOURCE = 1;
  auto obj = lookup_handle(source);
  if (!obj || !target) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  *target = proc().handles().insert(obj);
  if (options & DUPLICATE_CLOSE_SOURCE) proc().handles().close(source);
  return TRUE;
}

DWORD wait_multiple(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms) {
  if (count == 0 || count > 64 || !handles) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return WAIT_FAILED;
  }
  std::vector<std::shared_ptr<KernelObject>> objs;
  objs.reserve(count);
  for (DWORD i = 0; i < count; ++i) {
    if (handles[i] == kCurrentProcessPseudo) {
      // Waiting for our own process never completes.
      if (ms == INFINITE) for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      return WAIT_TIMEOUT;
    }
    auto o = lookup_handle(handles[i]);
    if (!o || !o->waitable()) {
      set_last_error(ERROR_INVALID_HANDLE);
      return WAIT_FAILED;
    }
    objs.push_back(std::move(o));
  }
  return rt::wait_for_objects(objs, wait_all != FALSE, ms, current_tid());
}

DWORD XBEM_GUEST_ABI hle_WaitForSingleObject(HANDLE h, DWORD ms) { return wait_multiple(1, &h, TRUE, ms); }
DWORD XBEM_GUEST_ABI hle_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return wait_multiple(1, &h, TRUE, ms); }
DWORD XBEM_GUEST_ABI hle_WaitForMultipleObjects(DWORD n, const HANDLE* h, BOOL all, DWORD ms) {
  return wait_multiple(n, h, all, ms);
}
DWORD XBEM_GUEST_ABI hle_WaitForMultipleObjectsEx(DWORD n, const HANDLE* h, BOOL all, DWORD ms, BOOL) {
  return wait_multiple(n, h, all, ms);
}

// ------------------------------------------------------------ kernel objects

HANDLE make_event(bool manual, bool initial, const std::string& name) {
  if (!name.empty()) XBEM_DEBUG("kernelx", "named event \"{}\" created as unnamed", name);
  return proc().handles().insert(std::make_shared<rt::EventObject>(manual, initial));
}


namespace {
// XBEM_TRACE_HANDLE=0x...: log creation and signalling of that handle with
// the caller (finding who should signal an object a title waits for).
uint64_t traced_handle() {
  static const uint64_t h = [] {
    const char* v = std::getenv("XBEM_TRACE_HANDLE");
    return v ? std::strtoull(v, nullptr, 0) : 0ull;
  }();
  return h;
}
void trace_handle(uint64_t h, const char* what, uint64_t caller) {
  if (h && h == traced_handle()) XBEM_INFO("kernelx", "handle {:#x}: {} from {}", h, what, describe_code_address(caller));
}
}  // namespace

HANDLE XBEM_GUEST_ABI hle_CreateEventA(void*, BOOL manual, BOOL initial, LPCSTR name) {
  const HANDLE h = make_event(manual, initial, narrow(name));
  trace_handle(h, "CreateEvent", XBEM_RETURN_ADDRESS());
  return h;
}
HANDLE XBEM_GUEST_ABI hle_CreateEventW(void*, BOOL manual, BOOL initial, LPCWSTR name) {
  const HANDLE h = make_event(manual, initial, narrow(name));
  trace_handle(h, "CreateEvent", XBEM_RETURN_ADDRESS());
  return h;
}
HANDLE XBEM_GUEST_ABI hle_CreateEventExW(void*, LPCWSTR name, DWORD flags, DWORD) {
  const HANDLE h = make_event(flags & 1 /*MANUAL_RESET*/, flags & 2 /*INITIAL_SET*/, narrow(name));
  trace_handle(h, "CreateEvent", XBEM_RETURN_ADDRESS());
  return h;
}
HANDLE XBEM_GUEST_ABI hle_CreateEventExA(void*, LPCSTR name, DWORD flags, DWORD) {
  const HANDLE h = make_event(flags & 1, flags & 2, narrow(name));
  trace_handle(h, "CreateEvent", XBEM_RETURN_ADDRESS());
  return h;
}

BOOL XBEM_GUEST_ABI hle_SetEvent(HANDLE h) {
  auto e = proc().handles().get_as<rt::EventObject>(h);
  if (!e) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  trace_handle(h, "SetEvent", XBEM_RETURN_ADDRESS());
  e->set();
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_ResetEvent(HANDLE h) {
  auto e = proc().handles().get_as<rt::EventObject>(h);
  if (!e) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  e->reset();
  return TRUE;
}

HANDLE XBEM_GUEST_ABI hle_CreateSemaphoreW(void*, LONG initial, LONG maximum, LPCWSTR) {
  if (maximum <= 0 || initial < 0 || initial > maximum) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  const HANDLE h = proc().handles().insert(std::make_shared<rt::SemaphoreObject>(initial, maximum));
  trace_handle(h, "CreateSemaphore", XBEM_RETURN_ADDRESS());
  return h;
}
HANDLE XBEM_GUEST_ABI hle_CreateSemaphoreA(void* sa, LONG initial, LONG maximum, LPCSTR) {
  return hle_CreateSemaphoreW(sa, initial, maximum, nullptr);
}
HANDLE XBEM_GUEST_ABI hle_CreateSemaphoreExW(void* sa, LONG initial, LONG maximum, LPCWSTR, DWORD, DWORD) {
  return hle_CreateSemaphoreW(sa, initial, maximum, nullptr);
}

BOOL XBEM_GUEST_ABI hle_ReleaseSemaphore(HANDLE h, LONG count, LONG* previous) {
  auto s = proc().handles().get_as<rt::SemaphoreObject>(h);
  if (!s) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  trace_handle(h, "ReleaseSemaphore", XBEM_RETURN_ADDRESS());
  if (!s->release(count, previous)) {
    set_last_error(ERROR_TOO_MANY_POSTS);
    return FALSE;
  }
  return TRUE;
}

HANDLE XBEM_GUEST_ABI hle_CreateMutexW(void*, BOOL initial_owner, LPCWSTR) {
  return proc().handles().insert(std::make_shared<rt::MutexObject>(initial_owner ? current_tid() : 0));
}
HANDLE XBEM_GUEST_ABI hle_CreateMutexA(void* sa, BOOL initial_owner, LPCSTR) {
  return hle_CreateMutexW(sa, initial_owner, nullptr);
}
HANDLE XBEM_GUEST_ABI hle_CreateMutexExW(void* sa, LPCWSTR, DWORD flags, DWORD) {
  return hle_CreateMutexW(sa, flags & 1 /*CREATE_MUTEX_INITIAL_OWNER*/, nullptr);
}

BOOL XBEM_GUEST_ABI hle_ReleaseMutex(HANDLE h) {
  auto m = proc().handles().get_as<rt::MutexObject>(h);
  if (!m) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (!m->release(current_tid())) {
    set_last_error(ERROR_NOT_OWNER);
    return FALSE;
  }
  return TRUE;
}

// --------------------------------------------------------- critical sections
//
// RTL_CRITICAL_SECTION (40 bytes) layout:
//   +0 DebugInfo (we store a pointer to the host lock here)
//   +8 LockCount  +12 RecursionCount  +16 OwningThread  +24 LockSemaphore  +32 SpinCount

struct CsLayout {
  uint64_t debug_info;
  int32_t lock_count;
  int32_t recursion_count;
  uint64_t owning_thread;
  uint64_t lock_semaphore;
  uint64_t spin_count;
};
static_assert(sizeof(CsLayout) == 40);

std::mutex g_lazy_mutex;

std::recursive_timed_mutex* cs_lock(CsLayout* cs) {
  if (cs->debug_info) return reinterpret_cast<std::recursive_timed_mutex*>(cs->debug_info);
  // Used without initialisation (undefined on Windows, but be forgiving).
  std::lock_guard lock(g_lazy_mutex);
  if (!cs->debug_info) {
    XBEM_WARN_ONCE("kernelx", "critical section used without InitializeCriticalSection");
    cs->debug_info = reinterpret_cast<uint64_t>(new std::recursive_timed_mutex());
  }
  return reinterpret_cast<std::recursive_timed_mutex*>(cs->debug_info);
}

void init_cs(CsLayout* cs, DWORD spin) {
  cs->debug_info = reinterpret_cast<uint64_t>(new std::recursive_timed_mutex());
  cs->lock_count = -1;
  cs->recursion_count = 0;
  cs->owning_thread = 0;
  cs->lock_semaphore = 0;
  cs->spin_count = spin;
}

void XBEM_GUEST_ABI hle_InitializeCriticalSection(CsLayout* cs) { init_cs(cs, 0); }
BOOL XBEM_GUEST_ABI hle_InitializeCriticalSectionAndSpinCount(CsLayout* cs, DWORD spin) {
  init_cs(cs, spin);
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_InitializeCriticalSectionEx(CsLayout* cs, DWORD spin, DWORD) {
  init_cs(cs, spin);
  return TRUE;
}

void XBEM_GUEST_ABI hle_EnterCriticalSection(CsLayout* cs) {
  cs_lock(cs)->lock();
  cs->owning_thread = current_tid();
  ++cs->recursion_count;
}

BOOL XBEM_GUEST_ABI hle_TryEnterCriticalSection(CsLayout* cs) {
  if (!cs_lock(cs)->try_lock()) return FALSE;
  cs->owning_thread = current_tid();
  ++cs->recursion_count;
  return TRUE;
}

void XBEM_GUEST_ABI hle_LeaveCriticalSection(CsLayout* cs) {
  if (--cs->recursion_count == 0) cs->owning_thread = 0;
  cs_lock(cs)->unlock();
}

void XBEM_GUEST_ABI hle_DeleteCriticalSection(CsLayout* cs) {
  delete reinterpret_cast<std::recursive_timed_mutex*>(cs->debug_info);
  cs->debug_info = 0;
}

DWORD XBEM_GUEST_ABI hle_SetCriticalSectionSpinCount(CsLayout* cs, DWORD spin) {
  const auto old = static_cast<DWORD>(cs->spin_count);
  cs->spin_count = spin;
  return old;
}

// ---------------------------------------------------- SRW locks / cond vars
//
// SRWLOCK and CONDITION_VARIABLE are pointer-sized, zero-initialised and may
// be used without any init call, so host objects are looked up by address.

template <class T>
T& keyed_object(const void* address) {
  static std::mutex mu;
  static std::unordered_map<const void*, std::unique_ptr<T>> objects;
  std::lock_guard lock(mu);
  auto& p = objects[address];
  if (!p) p = std::make_unique<T>();
  return *p;
}

std::shared_mutex& srw(const void* lock) { return keyed_object<std::shared_mutex>(lock); }
std::condition_variable_any& condvar(const void* cv) { return keyed_object<std::condition_variable_any>(cv); }

void XBEM_GUEST_ABI hle_InitializeSRWLock(uint64_t* lock) { *lock = 0; }
void XBEM_GUEST_ABI hle_AcquireSRWLockExclusive(uint64_t* lock) { srw(lock).lock(); }
void XBEM_GUEST_ABI hle_ReleaseSRWLockExclusive(uint64_t* lock) { srw(lock).unlock(); }
void XBEM_GUEST_ABI hle_AcquireSRWLockShared(uint64_t* lock) { srw(lock).lock_shared(); }
void XBEM_GUEST_ABI hle_ReleaseSRWLockShared(uint64_t* lock) { srw(lock).unlock_shared(); }
BOOLEAN XBEM_GUEST_ABI hle_TryAcquireSRWLockExclusive(uint64_t* lock) { return srw(lock).try_lock() ? 1 : 0; }
BOOLEAN XBEM_GUEST_ABI hle_TryAcquireSRWLockShared(uint64_t* lock) { return srw(lock).try_lock_shared() ? 1 : 0; }

void XBEM_GUEST_ABI hle_InitializeConditionVariable(uint64_t* cv) { *cv = 0; }
void XBEM_GUEST_ABI hle_WakeConditionVariable(uint64_t* cv) { condvar(cv).notify_one(); }
void XBEM_GUEST_ABI hle_WakeAllConditionVariable(uint64_t* cv) { condvar(cv).notify_all(); }

// Adapters giving condition_variable_any the BasicLockable it expects.
struct SharedLockAdapter {
  std::shared_mutex& m;
  void lock() { m.lock_shared(); }
  void unlock() { m.unlock_shared(); }
};

struct CsLockAdapter {
  CsLayout* cs;
  int32_t saved_recursion = 0;
  void unlock() {
    // A critical section may be held recursively; Sleep* releases it completely.
    saved_recursion = cs->recursion_count;
    cs->recursion_count = 0;
    cs->owning_thread = 0;
    for (int32_t i = 0; i < saved_recursion; ++i) cs_lock(cs)->unlock();
  }
  void lock() {
    for (int32_t i = 0; i < saved_recursion; ++i) cs_lock(cs)->lock();
    cs->recursion_count = saved_recursion;
    cs->owning_thread = current_tid();
  }
};

template <class Lock>
BOOL sleep_on(uint64_t* cv, Lock& lock, DWORD ms) {
  auto& c = condvar(cv);
  if (ms == INFINITE) {
    c.wait(lock);
    return TRUE;
  }
  if (c.wait_for(lock, std::chrono::milliseconds(ms)) == std::cv_status::timeout) {
    set_last_error(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SleepConditionVariableSRW(uint64_t* cv, uint64_t* lock, DWORD ms, ULONG flags) {
  constexpr ULONG CONDITION_VARIABLE_LOCKMODE_SHARED = 1;
  if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) {
    SharedLockAdapter a{srw(lock)};
    return sleep_on(cv, a, ms);
  }
  auto& m = srw(lock);
  return sleep_on(cv, m, ms);
}

BOOL XBEM_GUEST_ABI hle_SleepConditionVariableCS(uint64_t* cv, CsLayout* cs, DWORD ms) {
  CsLockAdapter a{cs};
  // condition_variable_any calls unlock() then lock(); our adapter is already "locked".
  return sleep_on(cv, a, ms);
}

// ---------------------------------------------------------------- init once

std::recursive_mutex g_init_once_mutex;
using InitOnceFn = BOOL(XBEM_GUEST_ABI*)(uint64_t*, void*, void**);

void XBEM_GUEST_ABI hle_InitOnceInitialize(uint64_t* once) { *once = 0; }

BOOL XBEM_GUEST_ABI hle_InitOnceExecuteOnce(uint64_t* once, uint64_t fn, void* param, void** context) {
  constexpr uint64_t kDone = 2;
  std::lock_guard lock(g_init_once_mutex);
  if (*once & kDone) {
    if (context) *context = reinterpret_cast<void*>(*once & ~3ull);
    return TRUE;
  }
  void* ctx = nullptr;
  if (!reinterpret_cast<InitOnceFn>(fn)(once, param, &ctx)) return FALSE;
  *once = (reinterpret_cast<uint64_t>(ctx) & ~3ull) | kDone;
  if (context) *context = ctx;
  return TRUE;
}

// ------------------------------------------------------------ WaitOnAddress
// Used by the MSVC STL for std::atomic::wait and many engine primitives.

BOOL XBEM_GUEST_ABI hle_WaitOnAddress(volatile void* address, const void* compare, SIZE_T size, DWORD ms) {
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  auto same = [&] { return std::memcmp(const_cast<const void*>(address), compare, size) == 0; };
  std::unique_lock lock(rt::sync_mutex());
  if (!same()) return TRUE;
  if (ms == INFINITE) {
    rt::sync_cv().wait(lock);  // spurious wake-ups are allowed by the API contract
    return TRUE;
  }
  if (rt::sync_cv().wait_for(lock, std::chrono::milliseconds(ms)) == std::cv_status::timeout && same()) {
    set_last_error(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

void XBEM_GUEST_ABI hle_WakeByAddressSingle(void*) {
  { std::lock_guard lock(rt::sync_mutex()); }
  rt::sync_cv().notify_all();
}

void XBEM_GUEST_ABI hle_WakeByAddressAll(void*) {
  { std::lock_guard lock(rt::sync_mutex()); }
  rt::sync_cv().notify_all();
}

// --------------------------------------------------------------- TLS / FLS

uint64_t* tls_slot(DWORD index, bool create) {
  GuestThread* t = rt::current_thread();
  if (!t || !t->teb) return nullptr;
  if (index < rt::teb::kMinimumSlots) return reinterpret_cast<uint64_t*>(t->teb + rt::teb::kTlsSlots + index * 8);
  if (index >= rt::teb::kMinimumSlots + rt::teb::kExpansionSlots) return nullptr;
  auto*& expansion = *reinterpret_cast<uint64_t**>(t->teb + rt::teb::kTlsExpansionSlots);
  if (!expansion) {
    if (!create) return nullptr;
    expansion = static_cast<uint64_t*>(std::calloc(rt::teb::kExpansionSlots, 8));
  }
  return expansion + (index - rt::teb::kMinimumSlots);
}

DWORD XBEM_GUEST_ABI hle_TlsAlloc() { return proc().tls_alloc(); }

BOOL XBEM_GUEST_ABI hle_TlsFree(DWORD index) {
  if (!proc().tls_free(index)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  return TRUE;
}

void* XBEM_GUEST_ABI hle_TlsGetValue(DWORD index) {
  uint64_t* slot = tls_slot(index, false);
  if (!slot && index >= rt::teb::kMinimumSlots + rt::teb::kExpansionSlots) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  set_last_error(ERROR_SUCCESS);  // documented behaviour: lets callers tell NULL values from errors
  return slot ? reinterpret_cast<void*>(*slot) : nullptr;
}

BOOL XBEM_GUEST_ABI hle_TlsSetValue(DWORD index, void* value) {
  uint64_t* slot = tls_slot(index, true);
  if (!slot) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  *slot = reinterpret_cast<uint64_t>(value);
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_FlsAlloc(uint64_t callback) { return proc().fls_alloc(callback); }

BOOL XBEM_GUEST_ABI hle_FlsFree(DWORD index) {
  if (!proc().fls_free(index)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  return TRUE;
}

void* XBEM_GUEST_ABI hle_FlsGetValue(DWORD index) {
  GuestThread* t = rt::current_thread();
  set_last_error(ERROR_SUCCESS);
  if (!t || index >= t->fls_values.size()) return nullptr;
  return reinterpret_cast<void*>(t->fls_values[index]);
}

BOOL XBEM_GUEST_ABI hle_FlsSetValue(DWORD index, void* value) {
  GuestThread* t = rt::current_thread();
  if (!t || index >= 4080) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (t->fls_values.size() <= index) t->fls_values.resize(index + 1, 0);
  t->fls_values[index] = reinterpret_cast<uint64_t>(value);
  return TRUE;
}

}  // namespace

void add_kernelx_sync_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(CreateThread), XBEM_HLE(ExitThread), XBEM_HLE(GetExitCodeThread), XBEM_HLE(GetThreadId),
      XBEM_HLE(ResumeThread), XBEM_HLE(SuspendThread), XBEM_HLE(SetThreadPriority), XBEM_HLE(GetThreadPriority),
      XBEM_HLE(SetThreadAffinityMask), XBEM_HLE(SetThreadIdealProcessor), XBEM_HLE(SetThreadDescription),
      XBEM_HLE(CloseHandle), XBEM_HLE(DuplicateHandle), XBEM_HLE(WaitForSingleObject),
      XBEM_HLE(WaitForSingleObjectEx), XBEM_HLE(WaitForMultipleObjects), XBEM_HLE(WaitForMultipleObjectsEx),
      XBEM_HLE(CreateEventA), XBEM_HLE(CreateEventW), XBEM_HLE(CreateEventExA), XBEM_HLE(CreateEventExW),
      XBEM_HLE(SetEvent), XBEM_HLE(ResetEvent), XBEM_HLE(CreateSemaphoreA), XBEM_HLE(CreateSemaphoreW),
      XBEM_HLE(CreateSemaphoreExW), XBEM_HLE(ReleaseSemaphore), XBEM_HLE(CreateMutexA), XBEM_HLE(CreateMutexW),
      XBEM_HLE(CreateMutexExW), XBEM_HLE(ReleaseMutex), XBEM_HLE(InitializeCriticalSection),
      XBEM_HLE(InitializeCriticalSectionAndSpinCount), XBEM_HLE(InitializeCriticalSectionEx),
      XBEM_HLE(EnterCriticalSection), XBEM_HLE(TryEnterCriticalSection), XBEM_HLE(LeaveCriticalSection),
      XBEM_HLE(DeleteCriticalSection), XBEM_HLE(SetCriticalSectionSpinCount), XBEM_HLE(InitializeSRWLock),
      XBEM_HLE(AcquireSRWLockExclusive), XBEM_HLE(ReleaseSRWLockExclusive), XBEM_HLE(AcquireSRWLockShared),
      XBEM_HLE(ReleaseSRWLockShared), XBEM_HLE(TryAcquireSRWLockExclusive), XBEM_HLE(TryAcquireSRWLockShared),
      XBEM_HLE(InitializeConditionVariable), XBEM_HLE(WakeConditionVariable), XBEM_HLE(WakeAllConditionVariable),
      XBEM_HLE(SleepConditionVariableSRW), XBEM_HLE(SleepConditionVariableCS), XBEM_HLE(InitOnceInitialize),
      XBEM_HLE(InitOnceExecuteOnce), XBEM_HLE(WaitOnAddress), XBEM_HLE(WakeByAddressSingle),
      XBEM_HLE(WakeByAddressAll), XBEM_HLE(TlsAlloc), XBEM_HLE(TlsFree), XBEM_HLE(TlsGetValue),
      XBEM_HLE(TlsSetValue), XBEM_HLE(FlsAlloc), XBEM_HLE(FlsFree), XBEM_HLE(FlsGetValue), XBEM_HLE(FlsSetValue),
  });
}

}  // namespace xbem::hle
