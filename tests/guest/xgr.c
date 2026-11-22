/* XGameRuntime: device type and XTaskQueue dispatch modes. */
#include "guest.h"

static volatile int g_manual_calls, g_immediate_calls, g_pool_calls;
static HANDLE g_pool_event;
static DWORD g_main_thread;
static volatile DWORD g_pool_thread;

static void on_manual(void* ctx, _Bool canceled) {
  (void)canceled;
  if (ctx == (void*)0x1234) ATOMIC_INC(&g_manual_calls);
}

static void on_immediate(void* ctx, _Bool canceled) {
  (void)ctx;
  (void)canceled;
  ATOMIC_INC(&g_immediate_calls);
}

static void on_pool(void* ctx, _Bool canceled) {
  (void)ctx;
  (void)canceled;
  g_pool_thread = GetCurrentThreadId();
  ATOMIC_INC(&g_pool_calls);
  SetEvent(g_pool_event);
}

DWORD guest_main(void* peb) {
  (void)peb;
  g_main_thread = GetCurrentThreadId();
  CHECK(XGameRuntimeInitialize() == 0);

  uint32_t device = XSystemGetDeviceType();
  say_num("device type", device);
  CHECK(device == 6 || device == 7); /* Series S (Lockhart) or Series X (Anaconda) */

  /* Manual: nothing runs until the title dispatches. */
  void* q = 0;
  CHECK(XTaskQueueCreate(0 /*Manual*/, 0, &q) == 0 && q != 0);
  CHECK(XTaskQueueSubmitCallback(q, 0 /*Work*/, (void*)0x1234, on_manual) == 0);
  CHECK(g_manual_calls == 0);
  CHECK(XTaskQueueDispatch(q, 0, 0));
  CHECK(g_manual_calls == 1);
  CHECK(!XTaskQueueDispatch(q, 0, 0)); /* empty now */
  XTaskQueueCloseHandle(q);

  /* Immediate: runs on the submitting thread. */
  CHECK(XTaskQueueCreate(3 /*Immediate*/, 3, &q) == 0);
  CHECK(XTaskQueueSubmitCallback(q, 0, 0, on_immediate) == 0);
  CHECK(g_immediate_calls == 1);
  XTaskQueueCloseHandle(q);

  /* Thread pool: runs on a worker (guest) thread. */
  g_pool_event = CreateEventW(0, 0, 0, 0);
  CHECK(XTaskQueueCreate(1 /*ThreadPool*/, 1, &q) == 0);
  CHECK(XTaskQueueSubmitCallback(q, 0, 0, on_pool) == 0);
  CHECK(WaitForSingleObject(g_pool_event, 5000) == WAIT_OBJECT_0);
  CHECK(g_pool_calls == 1);
  CHECK(g_pool_thread != 0 && g_pool_thread != g_main_thread);

  /* Network/user services are reported as not implemented. */
  CHECK(XUserAddAsync(0, 0) == (HRESULT)0x80004001);

  return (DWORD)finish("xgr");
}
