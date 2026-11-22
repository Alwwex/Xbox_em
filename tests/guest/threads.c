/* Threads, waits and synchronisation primitives. */
#include "guest.h"

#define NUM_THREADS 4
#define ITERATIONS 20000

static uint64_t g_cs[5];          /* CRITICAL_SECTION is 40 bytes */
static volatile int64_t g_counter; /* protected by g_cs */
static volatile int g_atomic_counter;
static DWORD g_tls_index;
static DWORD g_thread_ids[NUM_THREADS];

static uint64_t g_srw;  /* SRWLOCK, zero-initialised */
static uint64_t g_cv;   /* CONDITION_VARIABLE */
static volatile int g_ready;
static volatile int g_consumed;

static DWORD worker(void* arg) {
  const int index = (int)(intptr_t)arg;
  g_thread_ids[index] = GetCurrentThreadId();
  TlsSetValue(g_tls_index, (void*)(intptr_t)(index + 100));
  for (int i = 0; i < ITERATIONS; ++i) {
    EnterCriticalSection(g_cs);
    g_counter = g_counter + 1;
    LeaveCriticalSection(g_cs);
    ATOMIC_INC(&g_atomic_counter);
  }
  if ((intptr_t)TlsGetValue(g_tls_index) != index + 100) return 999;
  return (DWORD)(index + 1);
}

static DWORD consumer(void* arg) {
  (void)arg;
  AcquireSRWLockExclusive(&g_srw);
  while (!g_ready) SleepConditionVariableSRW(&g_cv, &g_srw, INFINITE, 0);
  g_consumed = 1;
  ReleaseSRWLockExclusive(&g_srw);
  return 0;
}

static DWORD exits_early(void* arg) {
  (void)arg;
  ExitThread(77);
  return 1; /* not reached */
}

static DWORD waits_for_event(void* event) {
  return WaitForSingleObject((HANDLE)event, 5000) == WAIT_OBJECT_0 ? 5 : 6;
}

DWORD guest_main(void* peb) {
  (void)peb;
  InitializeCriticalSection(g_cs);
  g_tls_index = TlsAlloc();
  CHECK(g_tls_index != 0xFFFFFFFFu);
  TlsSetValue(g_tls_index, (void*)42);

  HANDLE threads[NUM_THREADS];
  for (int i = 0; i < NUM_THREADS; ++i) {
    DWORD tid = 0;
    threads[i] = CreateThread(0, 0, worker, (void*)(intptr_t)i, 0, &tid);
    CHECK(threads[i] != 0);
    CHECK(tid != 0);
  }
  CHECK(WaitForMultipleObjects(NUM_THREADS, threads, 1, 10000) == WAIT_OBJECT_0);
  for (int i = 0; i < NUM_THREADS; ++i) {
    DWORD code = 0;
    CHECK(GetExitCodeThread(threads[i], &code));
    CHECK(code == (DWORD)(i + 1));
    CHECK(CloseHandle(threads[i]));
    for (int j = 0; j < i; ++j) CHECK(g_thread_ids[i] != g_thread_ids[j]);
  }
  CHECK(g_counter == (int64_t)NUM_THREADS * ITERATIONS);
  CHECK(ATOMIC_LOAD(&g_atomic_counter) == NUM_THREADS * ITERATIONS);
  CHECK((intptr_t)TlsGetValue(g_tls_index) == 42); /* main thread's slot untouched */
  DeleteCriticalSection(g_cs);

  /* Condition variable + SRW lock. */
  HANDLE c = CreateThread(0, 0, consumer, 0, 0, 0);
  Sleep(20);
  AcquireSRWLockExclusive(&g_srw);
  g_ready = 1;
  WakeAllConditionVariable(&g_cv);
  ReleaseSRWLockExclusive(&g_srw);
  CHECK(WaitForSingleObject(c, 5000) == WAIT_OBJECT_0);
  CHECK(g_consumed == 1);
  CloseHandle(c);

  /* ExitThread. */
  HANDLE e = CreateThread(0, 0, exits_early, 0, 0, 0);
  CHECK(WaitForSingleObject(e, 5000) == WAIT_OBJECT_0);
  DWORD code = 0;
  CHECK(GetExitCodeThread(e, &code) && code == 77);
  CloseHandle(e);

  /* Events: auto-reset event wakes exactly one waiter; timeouts work. */
  HANDLE ev = CreateEventW(0, 0, 0, 0);
  CHECK(WaitForSingleObject(ev, 10) == WAIT_TIMEOUT);
  HANDLE w = CreateThread(0, 0, waits_for_event, ev, 0, 0);
  Sleep(10);
  CHECK(SetEvent(ev));
  CHECK(WaitForSingleObject(w, 5000) == WAIT_OBJECT_0);
  CHECK(GetExitCodeThread(w, &code) && code == 5);
  CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT); /* consumed by the waiter */
  CloseHandle(w);
  CloseHandle(ev);

  return (DWORD)finish("threads");
}
