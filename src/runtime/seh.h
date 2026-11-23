#pragma once

// Windows x64 structured exception handling (SEH) for guest code.
//
// MSVC C++ exceptions, __try/__except/__finally and the MinGW/GCC unwinder are
// all built on the same OS primitives: RaiseException dispatches an exception
// by walking the stack with the unwind data in .pdata/.xdata and calling each
// frame's language handler; handlers then call RtlUnwindEx to unwind to the
// frame that catches. This module implements those primitives on top of the
// guest's own unwind tables.
//
// Host (xbem) frames on the guest stack have no Windows unwind data. Where an
// unwind walk reaches one, it continues from state recorded by the dispatcher
// that called the handler (see "active records" in seh.cpp), which mirrors
// what Windows does with its own dispatcher frames.

#include <cstddef>
#include <cstdint>

#include "hle/guest_types.h"

namespace xbem::seh {

struct alignas(16) M128 {
  uint64_t lo, hi;
};

// x64 CONTEXT (0x4D0 bytes). Gpr[] follows the unwind-code register numbering:
// RAX RCX RDX RBX RSP RBP RSI RDI R8..R15.
struct alignas(16) Context {
  uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
  uint32_t ContextFlags;
  uint32_t MxCsr;
  uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
  uint32_t EFlags;
  uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
  uint64_t Gpr[16];
  uint64_t Rip;
  uint8_t FltSaveLegacy[0xA0];  // XMM_SAVE_AREA32 up to XmmRegisters (ControlWord at +0, MxCsr at +0x18)
  M128 Xmm[16];
  uint8_t FltSaveReserved[0x60];
  M128 VectorRegister[26];
  uint64_t VectorControl;
  uint64_t DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
};

enum Reg : int { kRax = 0, kRcx, kRdx, kRbx, kRsp, kRbp, kRsi, kRdi, kR8, kR9, kR10, kR11, kR12, kR13, kR14, kR15 };

struct ExceptionRecord {
  uint32_t Code;
  uint32_t Flags;
  uint64_t Record;   // EXCEPTION_RECORD* of a nested exception
  uint64_t Address;
  uint32_t NumberParameters;
  uint32_t Pad;
  uint64_t Information[15];
};

struct RuntimeFunction {
  uint32_t BeginAddress, EndAddress, UnwindData;
};

struct DispatcherContext {
  uint64_t ControlPc;
  uint64_t ImageBase;
  const RuntimeFunction* FunctionEntry;
  uint64_t EstablisherFrame;
  uint64_t TargetIp;
  Context* ContextRecord;
  void* LanguageHandler;
  void* HandlerData;
  void* HistoryTable;
  uint32_t ScopeIndex;
  uint32_t Fill0;
};

struct ExceptionPointers {
  ExceptionRecord* record;
  Context* context;
};

// KNONVOLATILE_CONTEXT_POINTERS
struct ContextPointers {
  M128* Floating[16];
  uint64_t* Integer[16];
};

constexpr uint32_t kUnwFlagNHandler = 0;
constexpr uint32_t kUnwFlagEHandler = 1;
constexpr uint32_t kUnwFlagUHandler = 2;
constexpr uint32_t kUnwFlagChainInfo = 4;

constexpr uint32_t kExceptionNoncontinuable = 0x01;
constexpr uint32_t kExceptionUnwinding = 0x02;
constexpr uint32_t kExceptionExitUnwind = 0x04;
constexpr uint32_t kExceptionStackInvalid = 0x08;
constexpr uint32_t kExceptionNestedCall = 0x10;
constexpr uint32_t kExceptionTargetUnwind = 0x20;
constexpr uint32_t kExceptionCollidedUnwind = 0x40;

constexpr uint32_t kStatusLongjump = 0x80000026;
constexpr uint32_t kStatusUnwindConsolidate = 0x80000029;
constexpr uint32_t kStatusUnwind = 0xC0000027;
constexpr uint32_t kStatusNoncontinuableException = 0xC0000025;
constexpr uint32_t kStatusInvalidDisposition = 0xC0000026;
constexpr uint32_t kStatusAccessViolation = 0xC0000005;
constexpr uint32_t kExceptionMsvcCpp = 0xE06D7363;
constexpr uint32_t kExceptionGccThrow = 0x20474343;

enum Disposition : int32_t {
  kContinueExecution = 0,
  kContinueSearch = 1,
  kNestedException = 2,
  kCollidedUnwind = 3,
};

using LanguageHandler = int32_t(XBEM_GUEST_ABI*)(ExceptionRecord*, uint64_t frame, Context*, DispatcherContext*);
using VectoredHandler = int32_t(XBEM_GUEST_ABI*)(ExceptionPointers*);
using ConsolidateCallback = uint64_t(XBEM_GUEST_ABI*)(ExceptionRecord*);

// .pdata lookup in loaded guest modules and dynamically registered tables.
const RuntimeFunction* lookup_function_entry(uint64_t pc, uint64_t* image_base);
bool is_guest_code(uint64_t pc);

// RtlVirtualUnwind semantics. Returns the language handler matching `type`.
void* virtual_unwind(uint32_t type, uint64_t image_base, uint64_t pc, const RuntimeFunction* function, Context* ctx,
                     void** handler_data, uint64_t* establisher_frame, ContextPointers* pointers);

// Unwinds one frame of any kind (guest function, leaf, consolidation
// trampoline). Returns false at host code, i.e. the end of the guest stack.
bool unwind_one_frame(uint32_t type, DispatcherContext& dc, Context& ctx);

[[noreturn]] void raise_exception(ExceptionRecord* record, Context* context);
[[noreturn]] void unwind_ex(uint64_t end_frame, uint64_t target_ip, ExceptionRecord* record, uint64_t return_value,
                            Context* scratch, const Context& caller);
[[noreturn]] void restore_context(Context* context, ExceptionRecord* record);
uint16_t capture_back_trace(const Context& from, uint32_t skip, uint32_t count, uint64_t* out, uint32_t* hash);

bool add_function_table(const RuntimeFunction* table, uint32_t count, uint64_t base);
bool delete_function_table(const RuntimeFunction* table);

uint64_t add_vectored_handler(bool first, uint64_t handler, bool continue_handler);
bool remove_vectored_handler(uint64_t handle, bool continue_handler);
uint64_t set_unhandled_exception_filter(uint64_t filter);

// Human-readable description of an exception code (for logs and reports).
const char* exception_name(uint32_t code);

}  // namespace xbem::seh

// Assembly entry points (runtime/seh_asm.cpp). Guest-callable thunks capture
// the caller's register state before entering C++.
extern "C" {
void xbem_seh_RaiseException();
void xbem_seh_RtlRaiseException();
void xbem_seh_RtlCaptureContext();
void xbem_seh_RtlUnwindEx();
void xbem_seh_RtlUnwind();
void xbem_seh_RtlCaptureStackBackTrace();
void xbem_seh_consolidate_return();
[[noreturn]] void xbem_seh_restore_context_asm(const xbem::seh::Context* context);
uint64_t xbem_seh_call_consolidate(uint64_t callback, xbem::seh::ExceptionRecord* record,
                                   const xbem::seh::Context* target);
}
