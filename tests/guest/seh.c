/* Structured exception handling, the way the MSVC C++ runtime uses it.
 *
 * Functions below attach language handlers to their unwind data with the
 * `.seh_handler` directive. The "catching" handler does what
 * __CxxFrameHandler does for a matching catch clause: it calls RtlUnwindEx
 * with a STATUS_UNWIND_CONSOLIDATE record whose callback runs the "catch
 * block" and returns the address where execution continues.
 *
 * The "resume" labels stand in for the code after a catch block; `asm goto`
 * keeps GCC from deleting them as unreachable. */
#include "guest.h"

typedef struct {
  uint32_t Code, Flags;
  uint64_t Record, Address;
  uint32_t NumberParameters, Pad;
  uint64_t Information[15];
} EXREC;

typedef struct {
  uint64_t ControlPc, ImageBase;
  void* FunctionEntry;
  uint64_t EstablisherFrame, TargetIp;
  void* ContextRecord;
  void* LanguageHandler;
  void* HandlerData;
  void* HistoryTable;
  uint32_t ScopeIndex, Fill0;
} DISPCTX;

typedef struct __attribute__((aligned(16))) {
  uint64_t home[6];
  uint32_t flags, mxcsr;
  uint16_t seg[6];
  uint32_t eflags;
  uint64_t dr[6];
  uint64_t gpr[16];
  uint64_t rip;
  uint8_t rest[0x4D0 - 0x100];
} CTX;

typedef struct {
  uint32_t begin, end, unwind;
} RUNTIME_FUNCTION;

IMP void RaiseException(DWORD code, DWORD flags, DWORD nargs, const uint64_t* args);
IMP void RtlUnwindEx(void* frame, void* target, EXREC* rec, void* retval, CTX* ctx, void* history);
IMP void RtlCaptureContext(CTX* ctx);
IMP RUNTIME_FUNCTION* RtlLookupFunctionEntry(uint64_t pc, uint64_t* base, void* history);
IMP void* RtlVirtualUnwind(DWORD type, uint64_t base, uint64_t pc, RUNTIME_FUNCTION* fn, CTX* ctx, void** data,
                           uint64_t* frame, void* pointers);
IMP uint16_t RtlCaptureStackBackTrace(DWORD skip, DWORD count, void** frames, DWORD* hash);
IMP void* AddVectoredExceptionHandler(DWORD first, long (*handler)(void*));
IMP DWORD RemoveVectoredExceptionHandler(void* handle);

#define CODE_INNER 0xE0001234u
#define CODE_OUTER 0xE0005678u
#define STATUS_UNWIND_CONSOLIDATE 0x80000029u
#define FLAG_UNWINDING 0x2u
#define FLAG_TARGET 0x20u

static volatile int g_cleanups, g_target_unwinds, g_caught, g_rethrow_caught, g_after, g_outer_after;
void* volatile g_resume;
void* volatile g_outer_resume;

/* ------------------------------------------------------------ handlers */

static uint64_t catch_block(EXREC* rec) {
  g_caught = (int)rec->Information[1];
  return (uint64_t)g_resume;
}

static uint64_t catch_block_that_rethrows(EXREC* rec) {
  g_caught = (int)rec->Information[1];
  uint64_t arg = 99;
  RaiseException(CODE_OUTER, 0, 1, &arg); /* thrown from inside the "catch block" */
  return 0;                               /* not reached */
}

static uint64_t outer_catch_block(EXREC* rec) {
  g_rethrow_caught = (int)rec->Information[1];
  return (uint64_t)g_outer_resume;
}

static int catch_with(EXREC* rec, uint64_t frame, DISPCTX* dc, uint32_t code, uint64_t (*block)(EXREC*)) {
  if (rec->Flags & FLAG_UNWINDING) {
    if (rec->Flags & FLAG_TARGET) ATOMIC_INC(&g_target_unwinds);
    return 1; /* ExceptionContinueSearch */
  }
  if (rec->Code != code) return 1;
  EXREC consolidate = {0};
  consolidate.Code = STATUS_UNWIND_CONSOLIDATE;
  consolidate.Flags = 1;
  consolidate.NumberParameters = 2;
  consolidate.Information[0] = (uint64_t)block;
  consolidate.Information[1] = rec->Information[0];
  CTX scratch;
  RtlUnwindEx((void*)frame, (void*)dc->ControlPc, &consolidate, 0, &scratch, dc->HistoryTable);
  return 1; /* not reached */
}

__attribute__((used)) int inner_handler(EXREC* rec, uint64_t frame, void* ctx, DISPCTX* dc) {
  (void)ctx;
  return catch_with(rec, frame, dc, CODE_INNER, catch_block);
}

__attribute__((used)) int rethrow_handler(EXREC* rec, uint64_t frame, void* ctx, DISPCTX* dc) {
  (void)ctx;
  return catch_with(rec, frame, dc, CODE_INNER, catch_block_that_rethrows);
}

__attribute__((used)) int outer_handler(EXREC* rec, uint64_t frame, void* ctx, DISPCTX* dc) {
  (void)ctx;
  return catch_with(rec, frame, dc, CODE_OUTER, outer_catch_block);
}

/* An unwind-only handler, like a frame with destructors to run. */
__attribute__((used)) int cleanup_handler(EXREC* rec, uint64_t frame, void* ctx, DISPCTX* dc) {
  (void)frame;
  (void)ctx;
  (void)dc;
  if (rec->Flags & FLAG_UNWINDING) ATOMIC_INC(&g_cleanups);
  return 1;
}

/* ------------------------------------------------------- frames */

__attribute__((noinline)) static void raiser(int value) {
  uint64_t arg = (uint64_t)value;
  RaiseException(CODE_INNER, 0, 1, &arg);
  __asm__ volatile("nop");
}

__attribute__((noinline)) static void middle(int value) {
  __asm__ volatile(".seh_handler cleanup_handler, @unwind");
  raiser(value);
  __asm__ volatile("nop"); /* no tail call: keep this frame on the stack */
}

__attribute__((noinline, optimize("O0"))) static int catcher(int value) {
  __asm__ volatile(".seh_handler inner_handler, @except, @unwind");
  g_resume = &&resume;
  __asm__ goto("" :::: resume); /* tells GCC the resume block is reachable */
  middle(value);
  return -1;
resume:
  g_after = 1;
  return 0;
}

__attribute__((noinline, optimize("O0"))) static int catcher_rethrow(int value) {
  __asm__ volatile(".seh_handler rethrow_handler, @except, @unwind");
  g_resume = &&resume;
  __asm__ goto("" :::: resume);
  middle(value);
  return -1;
resume:
  return -2; /* the catch block rethrows, so this is not reached either */
}

__attribute__((noinline, optimize("O0"))) static int outer_catcher(int value) {
  __asm__ volatile(".seh_handler outer_handler, @except, @unwind");
  g_outer_resume = &&resume;
  __asm__ goto("" :::: resume);
  catcher_rethrow(value);
  return -1;
resume:
  g_outer_after = 1;
  return 0;
}

/* ---------------------------------------------- introspection helpers */

static volatile uint64_t g_expected_return;

__attribute__((noinline)) static int unwind_one_frame(void) {
  CTX ctx;
  RtlCaptureContext(&ctx);
  g_expected_return = (uint64_t)__builtin_return_address(0);
  uint64_t base = 0, frame = 0;
  void* data = 0;
  RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.rip, &base, 0);
  if (!fn) return 0;
  RtlVirtualUnwind(0, base, ctx.rip, fn, &ctx, &data, &frame, 0);
  __asm__ volatile("nop");
  return ctx.rip == g_expected_return;
}

__attribute__((noinline)) static int backtrace_check(void) {
  void* frames[8];
  DWORD hash = 0;
  uint16_t n = RtlCaptureStackBackTrace(0, 8, frames, &hash);
  int ok = n >= 2 && frames[1] == __builtin_return_address(0) && hash != 0;
  __asm__ volatile("nop");
  return ok;
}

static volatile int g_veh_seen;
static long veh(void* pointers) {
  EXREC* rec = *(EXREC**)pointers;
  if (rec->Code != 0xE000AAAAu) return 0; /* EXCEPTION_CONTINUE_SEARCH */
  ATOMIC_INC(&g_veh_seen);
  return -1; /* EXCEPTION_CONTINUE_EXECUTION */
}

DWORD guest_main(void* peb) {
  (void)peb;

  /* 1. Dispatch + consolidated unwind to a catching frame. */
  CHECK(catcher(7) == 0);
  CHECK(g_caught == 7);
  CHECK(g_after == 1);
  CHECK(g_cleanups == 1);       /* middle's unwind handler ran once */
  CHECK(g_target_unwinds == 1); /* catcher saw EXCEPTION_TARGET_UNWIND */

  /* 2. A second exception raised inside the "catch block" (rethrow) must
   *    unwind through the consolidation frame to an outer catcher. */
  g_caught = g_cleanups = g_target_unwinds = 0;
  CHECK(outer_catcher(11) == 0);
  CHECK(g_caught == 11);
  CHECK(g_rethrow_caught == 99);
  CHECK(g_outer_after == 1);
  CHECK(g_cleanups == 1);
  CHECK(g_target_unwinds == 2); /* catcher_rethrow (first unwind) + outer_catcher */

  /* 3. Stack introspection used by crash reporters and allocators. */
  CHECK(unwind_one_frame());
  CHECK(backtrace_check());

  /* 4. Vectored handlers run first and may resume execution. */
  void* h = AddVectoredExceptionHandler(1, veh);
  CHECK(h != 0);
  RaiseException(0xE000AAAAu, 0, 0, 0);
  CHECK(g_veh_seen == 1);
  CHECK(RemoveVectoredExceptionHandler(h) == 1);

  return (DWORD)finish("seh");
}
