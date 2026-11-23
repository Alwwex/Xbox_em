/* Start-up services found in real titles: registry (empty), ETW, WinRT
 * strings, COM basics, BSTR, random numbers and the Windows thread pool. */
#include "guest.h"

typedef uint16_t wchar;
#define W(s) ((const wchar*)L##s)

IMP long RegOpenKeyExW(void* key, const wchar* sub, DWORD, DWORD, void** out);
IMP long RegQueryValueExW(void* key, const wchar* name, DWORD*, DWORD*, void*, DWORD*);
IMP long RegCloseKey(void*);
IMP unsigned long EventRegister(const void* guid, void* cb, void* ctx, uint64_t* handle);
IMP long BCryptGenRandom(void* alg, void* buf, unsigned long len, unsigned long flags);
IMP wchar* SysAllocString(const wchar*);
IMP unsigned SysStringLen(const wchar*);
IMP void SysFreeString(wchar*);
IMP HRESULT WindowsCreateString(const wchar*, unsigned, void**);
IMP HRESULT WindowsDeleteString(void*);
IMP HRESULT WindowsDuplicateString(void*, void**);
IMP const wchar* WindowsGetStringRawBuffer(void*, unsigned*);
IMP HRESULT WindowsCompareStringOrdinal(void*, void*, int*);
IMP HRESULT CoCreateGuid(uint8_t*);
IMP int StringFromGUID2(const uint8_t*, wchar*, int);
IMP HRESULT CLSIDFromString(const wchar*, uint8_t*);
IMP HRESULT CoCreateInstance(const void*, void*, DWORD, const void*, void**);

typedef void (*WORK_CB)(void* inst, void* ctx, void* work);
typedef void (*WAIT_CB)(void* inst, void* ctx, void* wait, DWORD result);
typedef void (*SIMPLE_CB)(void* inst, void* ctx);
IMP void* CreateThreadpoolWork(WORK_CB, void*, void* env);
IMP void SubmitThreadpoolWork(void*);
IMP void WaitForThreadpoolWorkCallbacks(void*, BOOL);
IMP void CloseThreadpoolWork(void*);
IMP void* CreateThreadpoolTimer(WORK_CB, void*, void* env);
IMP void SetThreadpoolTimer(void*, const int64_t* due, DWORD period, DWORD window);
IMP void WaitForThreadpoolTimerCallbacks(void*, BOOL);
IMP void CloseThreadpoolTimer(void*);
IMP void* CreateThreadpoolWait(WAIT_CB, void*, void* env);
IMP void SetThreadpoolWait(void*, HANDLE, const int64_t* timeout);
IMP void WaitForThreadpoolWaitCallbacks(void*, BOOL);
IMP void CloseThreadpoolWait(void*);
IMP BOOL TrySubmitThreadpoolCallback(SIMPLE_CB, void*, void* env);
IMP void SetEventWhenCallbackReturns(void* inst, HANDLE);
IMP void* CreateThreadpoolCleanupGroup(void);
IMP void CloseThreadpoolCleanupGroupMembers(void*, BOOL, void*);
IMP void CloseThreadpoolCleanupGroup(void*);

static volatile int g_work_count;
static volatile DWORD g_main_tid, g_worker_tid;
static volatile int g_timer_count;
static volatile DWORD g_wait_results[2];
static volatile int g_wait_calls;

static void work_cb(void* inst, void* ctx, void* work) {
  (void)inst;
  (void)work;
  ATOMIC_INC((volatile int*)ctx);
  g_worker_tid = GetCurrentThreadId();
}

static void timer_cb(void* inst, void* ctx, void* timer) {
  (void)ctx;
  (void)timer;
  ATOMIC_INC(&g_timer_count);
  SetEventWhenCallbackReturns(inst, (HANDLE)ctx);
}

static void wait_cb(void* inst, void* ctx, void* wait, DWORD result) {
  (void)inst;
  (void)ctx;
  (void)wait;
  int i = ATOMIC_INC(&g_wait_calls) - 1;
  if (i < 2) g_wait_results[i] = result;
}

static void simple_cb(void* inst, void* ctx) { SetEventWhenCallbackReturns(inst, (HANDLE)ctx); }

DWORD guest_main(void* peb) {
  (void)peb;
  g_main_tid = GetCurrentThreadId();

  /* Registry: nothing there, and callers must not see "success". */
  void* key = (void*)1;
  CHECK(RegOpenKeyExW((void*)0x80000002, W("SOFTWARE\\Game"), 0, 0x20019, &key) == 2);
  CHECK(key == 0);
  CHECK(RegQueryValueExW(0, W("Value"), 0, 0, 0, 0) == 2);
  CHECK(RegCloseKey(0) == 0);

  uint64_t reg = 0;
  uint8_t provider[16] = {1, 2, 3};
  CHECK(EventRegister(provider, 0, 0, &reg) == 0 && reg != 0);

  uint8_t rnd[32] = {0};
  CHECK(BCryptGenRandom(0, rnd, sizeof rnd, 2) == 0);
  int nonzero = 0;
  for (int i = 0; i < 32; ++i) nonzero += rnd[i] != 0;
  CHECK(nonzero > 16);

  wchar* bstr = SysAllocString(W("bstr"));
  CHECK(bstr && SysStringLen(bstr) == 4);
  SysFreeString(bstr);

  /* WinRT strings. */
  void *h1 = 0, *h2 = 0;
  CHECK(WindowsCreateString(W("Windows.Foo"), 11, &h1) == 0 && h1);
  CHECK(WindowsDuplicateString(h1, &h2) == 0 && h2 == h1);
  unsigned len = 0;
  const wchar* raw = WindowsGetStringRawBuffer(h2, &len);
  CHECK(len == 11 && raw[0] == 'W' && raw[11] == 0);
  int cmp = 9;
  CHECK(WindowsCompareStringOrdinal(h1, 0, &cmp) == 0 && cmp == 1);
  WindowsDeleteString(h2);
  WindowsDeleteString(h1);
  CHECK(WindowsGetStringRawBuffer(0, &len)[0] == 0 && len == 0);

  /* COM. */
  uint8_t guid[16], parsed[16];
  wchar text[40];
  CHECK(CoCreateGuid(guid) == 0);
  CHECK(StringFromGUID2(guid, text, 40) == 39 && text[0] == '{');
  CHECK(CLSIDFromString(text, parsed) == 0 && memcmp(guid, parsed, 16) == 0);
  void* obj = (void*)1;
  CHECK(CoCreateInstance(guid, 0, 1, guid, &obj) == (HRESULT)0x80040154 && obj == 0);

  /* Thread pool: work items run on worker threads. */
  void* work = CreateThreadpoolWork(work_cb, (void*)&g_work_count, 0);
  CHECK(work != 0);
  for (int i = 0; i < 100; ++i) SubmitThreadpoolWork(work);
  WaitForThreadpoolWorkCallbacks(work, 0);
  CHECK(g_work_count == 100);
  CHECK(g_worker_tid != 0 && g_worker_tid != g_main_tid);
  CloseThreadpoolWork(work);

  /* One-shot and periodic timers (relative due times, 100 ns units). */
  HANDLE timer_event = CreateEventW(0, 0, 0, 0);
  void* timer = CreateThreadpoolTimer(timer_cb, timer_event, 0);
  int64_t due = -100000; /* 10 ms */
  SetThreadpoolTimer(timer, &due, 0, 0);
  CHECK(WaitForSingleObject(timer_event, 2000) == WAIT_OBJECT_0);
  CHECK(g_timer_count == 1);
  SetThreadpoolTimer(timer, &due, 5, 0); /* every 5 ms */
  for (int i = 0; i < 3; ++i) CHECK(WaitForSingleObject(timer_event, 2000) == WAIT_OBJECT_0);
  SetThreadpoolTimer(timer, 0, 0, 0);
  WaitForThreadpoolTimerCallbacks(timer, 1);
  const int after_cancel = g_timer_count;
  Sleep(30);
  CHECK(g_timer_count == after_cancel);
  CloseThreadpoolTimer(timer);

  /* Waits: one signalled, one timing out. */
  HANDLE ev = CreateEventW(0, 0, 0, 0);
  void* wait = CreateThreadpoolWait(wait_cb, 0, 0);
  SetThreadpoolWait(wait, ev, 0);
  SetEvent(ev);
  for (int i = 0; i < 200 && g_wait_calls < 1; ++i) Sleep(5);
  WaitForThreadpoolWaitCallbacks(wait, 0);
  CHECK(g_wait_calls == 1 && g_wait_results[0] == WAIT_OBJECT_0);
  int64_t timeout = -200000; /* 20 ms */
  SetThreadpoolWait(wait, ev, &timeout);
  for (int i = 0; i < 200 && g_wait_calls < 2; ++i) Sleep(5);
  WaitForThreadpoolWaitCallbacks(wait, 0);
  CHECK(g_wait_calls == 2 && g_wait_results[1] == WAIT_TIMEOUT);
  CloseThreadpoolWait(wait);

  /* Simple callbacks + SetEventWhenCallbackReturns. */
  HANDLE done = CreateEventW(0, 1, 0, 0);
  CHECK(TrySubmitThreadpoolCallback(simple_cb, done, 0));
  CHECK(WaitForSingleObject(done, 2000) == WAIT_OBJECT_0);

  /* Cleanup groups: members are waited for and closed together. */
  struct {
    DWORD version;
    DWORD pad;
    void* pool;
    void* cleanup_group;
    void* rest[6];
  } env = {3, 0, 0, 0, {0}};
  env.cleanup_group = CreateThreadpoolCleanupGroup();
  static volatile int group_count;
  void* gw = CreateThreadpoolWork(work_cb, (void*)&group_count, &env);
  for (int i = 0; i < 10; ++i) SubmitThreadpoolWork(gw);
  CloseThreadpoolCleanupGroupMembers(env.cleanup_group, 0, 0);
  CHECK(group_count == 10);
  CloseThreadpoolCleanupGroup(env.cleanup_group);

  return (DWORD)finish("system");
}
