#include "runtime/seh.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "loader/pe_format.h"
#include "runtime/process.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define XBEM_FRAME_ADDRESS() reinterpret_cast<uintptr_t>(_AddressOfReturnAddress())
#else
#define XBEM_FRAME_ADDRESS() reinterpret_cast<uintptr_t>(__builtin_frame_address(0))
#endif

namespace xbem::seh {

static_assert(sizeof(Context) == 0x4D0);
static_assert(offsetof(Context, ContextFlags) == 0x30);
static_assert(offsetof(Context, MxCsr) == 0x34);
static_assert(offsetof(Context, SegCs) == 0x38);
static_assert(offsetof(Context, SegSs) == 0x42);
static_assert(offsetof(Context, EFlags) == 0x44);
static_assert(offsetof(Context, Gpr) == 0x78);
static_assert(offsetof(Context, Rip) == 0xF8);
static_assert(offsetof(Context, FltSaveLegacy) == 0x100);
static_assert(offsetof(Context, Xmm) == 0x1A0);
static_assert(offsetof(Context, VectorRegister) == 0x300);
static_assert(sizeof(ExceptionRecord) == 0x98);
static_assert(sizeof(DispatcherContext) == 80);

namespace {

constexpr int kMaxFrames = 100000;
constexpr size_t kConsolidateContextOffset = 0x20;  // see xbem_seh_call_consolidate

// ------------------------------------------------------------ unwind tables

struct DynamicTable {
  const RuntimeFunction* table;
  uint32_t count;
  uint64_t base;
};
std::mutex g_tables_mutex;
std::vector<DynamicTable> g_dynamic_tables;

const RuntimeFunction* search_table(const RuntimeFunction* table, size_t count, uint32_t rva) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (rva < table[mid].BeginAddress) hi = mid;
    else if (rva >= table[mid].EndAddress) lo = mid + 1;
    else return &table[mid];
  }
  return nullptr;
}

// ----------------------------------------------- per-thread dispatch records
//
// While the dispatcher calls an exception handler, or RtlUnwindEx calls an
// unwind handler, host frames sit on the guest stack between the handler and
// the frames being processed. A walk that reaches such a host frame looks up
// the record that owns it (the innermost record above the walk position) and
// continues the way Windows' own dispatcher frames would make it continue:
//   unwind   -> dispatch record: continue from the raise point
//   unwind   -> unwind record:   collided unwind, take over its current frame
//   dispatch -> unwind record:   exception raised by an unwind handler; the
//                                search continues at the frame being unwound
//   dispatch -> dispatch record: nested exception; continue after the frame
//                                whose handler raised it

struct ActiveRecord {
  enum Kind { Dispatch, Unwind } kind;
  uintptr_t marker;              // host frame address; lower = more recent
  const Context* raise_point;    // Dispatch: private copy of the raise context
  DispatcherContext* dc;         // frame being processed (its ContextRecord included)
};

constexpr int kMaxRecords = 64;
thread_local ActiveRecord t_records[kMaxRecords];
thread_local int t_record_count = 0;

int push_record(ActiveRecord::Kind kind, uintptr_t marker, const Context* raise_point, DispatcherContext* dc) {
  if (t_record_count >= kMaxRecords) {
    XBEM_ERROR("seh", "exception nesting too deep");
    rt::Process::current().exit_process(kStatusNoncontinuableException);
  }
  t_records[t_record_count] = {kind, marker, raise_point, dc};
  return t_record_count++;
}

void pop_records(int index) { t_record_count = std::min(t_record_count, index); }

void purge_records_below(uint64_t rsp) {
  while (t_record_count > 0 && t_records[t_record_count - 1].marker < rsp) --t_record_count;
}

// The record whose host frame is the first one above stack position `rsp`.
const ActiveRecord* owner_of_host_frame(uint64_t rsp) {
  for (int i = t_record_count - 1; i >= 0; --i)
    if (t_records[i].marker > rsp) return &t_records[i];
  return nullptr;
}

void take_over_frame(DispatcherContext& dc, const DispatcherContext& o) {
  dc.ControlPc = o.ControlPc;
  dc.ImageBase = o.ImageBase;
  dc.FunctionEntry = o.FunctionEntry;
  dc.EstablisherFrame = o.EstablisherFrame;
  dc.LanguageHandler = o.LanguageHandler;
  dc.HandlerData = o.HandlerData;
  dc.ScopeIndex = o.ScopeIndex;
}

// ---------------------------------------------------------------- helpers

void set_int_reg(Context& c, ContextPointers* p, int reg, uint64_t* addr) {
  c.Gpr[reg] = *addr;
  if (p) p->Integer[reg] = addr;
}

void set_xmm_reg(Context& c, ContextPointers* p, int reg, M128* addr) {
  std::memcpy(&c.Xmm[reg], addr, sizeof(M128));
  if (p) p->Floating[reg] = addr;
}

bool valid_frame(uint64_t frame) {
  if (frame & 7) return false;
  rt::GuestThread* t = rt::current_thread();
  if (!t || !t->teb) return true;
  const uint64_t limit = *reinterpret_cast<const uint64_t*>(t->teb + rt::teb::kStackLimit);
  const uint64_t base = *reinterpret_cast<const uint64_t*>(t->teb + rt::teb::kStackBase);
  return frame >= limit - 0x1000 && frame <= base;  // pthread stacks start one page below StackLimit
}

[[noreturn]] void fatal(const char* what, uint32_t code) {
  XBEM_ERROR("seh", "{}", what);
  rt::Process::current().exit_process(code);
}

std::string describe(uint64_t address) {
  char buf[160] = {};
  if (rt::Process* p = rt::Process::current_or_null()) {
    if (p->loader().describe(address, buf, sizeof buf)) return buf;
  }
  return std::format("{:#x}", address);
}

// ------------------------------------------------------------ epilogs

int opcode_slots(uint8_t op, uint8_t info) {
  switch (op) {
    case 1: return info ? 3 : 2;  // UWOP_ALLOC_LARGE
    case 4:                        // UWOP_SAVE_NONVOL
    case 6:                        // UWOP_EPILOG (v2) / UWOP_SAVE_XMM (v1)
    case 8: return 2;              // UWOP_SAVE_XMM128
    case 5:                        // UWOP_SAVE_NONVOL_FAR
    case 7:                        // UWOP_SAVE_XMM_FAR (v1)
    case 9: return 3;              // UWOP_SAVE_XMM128_FAR
    default: return 1;
  }
}

// x64 epilogs have a fixed shape: [add rsp, n | lea rsp, [reg+n]] pop* (ret | jmp).
bool is_inside_epilog(const uint8_t* pc, uint64_t base, const RuntimeFunction* fn) {
  if ((pc[0] & 0xF8) == 0x48) {
    switch (pc[1]) {
      case 0x81:  // add rsp, imm32
        if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 7; break; }
        return false;
      case 0x83:  // add rsp, imm8
        if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 4; break; }
        return false;
      case 0x8D:  // lea rsp, [reg+disp]
        if (pc[0] & 0x06) return false;
        if (((pc[2] >> 3) & 7) != 4) return false;
        if ((pc[2] & 7) == 4) return false;
        if ((pc[2] >> 6) == 1) { pc += 4; break; }
        if ((pc[2] >> 6) == 2) { pc += 7; break; }
        return false;
    }
  }
  for (int guard = 0; guard < 64; ++guard) {
    if ((*pc & 0xF0) == 0x40) pc++;  // REX
    switch (*pc) {
      case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        pc++;
        continue;
      case 0xC2: case 0xC3:
        return true;
      case 0xE9: {
        int32_t rel;
        std::memcpy(&rel, pc + 1, 4);
        pc += 5 + rel;
        const uint64_t rva = reinterpret_cast<uint64_t>(pc) - base;
        if (rva >= fn->BeginAddress && rva < fn->EndAddress) continue;
        break;
      }
      case 0xEB:
        pc += 2 + static_cast<int8_t>(pc[1]);
        if (const uint64_t rva = reinterpret_cast<uint64_t>(pc) - base; rva >= fn->BeginAddress && rva < fn->EndAddress)
          continue;
        break;
      case 0xF3:
        return pc[1] == 0xC3;
    }
    return false;
  }
  return false;
}

void interpret_epilog(const uint8_t* pc, Context& c, ContextPointers* p) {
  for (int guard = 0; guard < 64; ++guard) {
    uint8_t rex = 0;
    if ((*pc & 0xF0) == 0x40) rex = *pc++ & 0x0F;
    switch (*pc) {
      case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        set_int_reg(c, p, *pc - 0x58 + (rex & 1) * 8, reinterpret_cast<uint64_t*>(c.Gpr[kRsp]));
        c.Gpr[kRsp] += 8;
        pc++;
        continue;
      case 0x81: {
        int32_t imm;
        std::memcpy(&imm, pc + 2, 4);
        c.Gpr[kRsp] += imm;
        pc += 6;
        continue;
      }
      case 0x83:
        c.Gpr[kRsp] += static_cast<int8_t>(pc[2]);
        pc += 3;
        continue;
      case 0x8D:
        if ((pc[1] >> 6) == 1) {
          c.Gpr[kRsp] = c.Gpr[(pc[1] & 7) + (rex & 1) * 8] + static_cast<int8_t>(pc[2]);
          pc += 3;
        } else {
          int32_t disp;
          std::memcpy(&disp, pc + 2, 4);
          c.Gpr[kRsp] = c.Gpr[(pc[1] & 7) + (rex & 1) * 8] + disp;
          pc += 6;
        }
        continue;
      case 0xC2: {
        uint16_t extra;
        std::memcpy(&extra, pc + 1, 2);
        c.Rip = *reinterpret_cast<uint64_t*>(c.Gpr[kRsp]);
        c.Gpr[kRsp] += 8 + extra;
        return;
      }
      case 0xC3:
      case 0xF3:
        c.Rip = *reinterpret_cast<uint64_t*>(c.Gpr[kRsp]);
        c.Gpr[kRsp] += 8;
        return;
      case 0xE9: {
        int32_t rel;
        std::memcpy(&rel, pc + 1, 4);
        pc += 5 + rel;
        continue;
      }
      case 0xEB:
        pc += 2 + static_cast<int8_t>(pc[1]);
        continue;
    }
    return;
  }
}

void redo_unwind(const DispatcherContext& dc, Context& c) {
  if (dc.FunctionEntry) {
    void* data = nullptr;
    uint64_t frame = 0;
    virtual_unwind(kUnwFlagNHandler, dc.ImageBase, dc.ControlPc, dc.FunctionEntry, &c, &data, &frame, nullptr);
  } else if (dc.ControlPc == reinterpret_cast<uint64_t>(&xbem_seh_consolidate_return)) {
    std::memcpy(&c, reinterpret_cast<const void*>(c.Gpr[kRsp] + kConsolidateContextOffset), sizeof(Context));
  } else {
    c.Rip = *reinterpret_cast<uint64_t*>(c.Gpr[kRsp]);
    c.Gpr[kRsp] += 8;
  }
}

// ------------------------------------------------------ vectored handlers

struct VectoredEntry {
  uint64_t id;
  uint64_t handler;
};
std::mutex g_veh_mutex;
std::vector<VectoredEntry> g_vectored, g_continue;
uint64_t g_next_veh_id = 0x1000;
uint64_t g_unhandled_filter = 0;

int32_t call_vectored(std::vector<VectoredEntry>& list, ExceptionRecord* rec, Context* ctx) {
  std::vector<VectoredEntry> copy;
  {
    std::lock_guard lock(g_veh_mutex);
    copy = list;
  }
  ExceptionPointers ptrs{rec, ctx};
  for (const auto& e : copy)
    if (reinterpret_cast<VectoredHandler>(e.handler)(&ptrs) == -1) return -1;  // EXCEPTION_CONTINUE_EXECUTION
  return 0;
}

// ------------------------------------------------------ reporting

bool readable_guest(uint64_t addr, size_t len) {
  rt::Process* p = rt::Process::current_or_null();
  if (!p) return false;
  rt::LoadedModule* m = p->loader().find_containing(addr);
  return m && !m->hle && m->contains(addr + len - 1);
}

std::string msvc_exception_type(const ExceptionRecord* rec) {
  // ExceptionInformation: [0] magic, [1] object, [2] ThrowInfo*, [3] image base (x64: RVAs)
  if (rec->NumberParameters < 4) return {};
  const uint64_t base = rec->Information[3];
  const uint64_t throw_info = rec->Information[2];
  if (!readable_guest(throw_info, 16)) return {};
  int32_t cta_rva;
  std::memcpy(&cta_rva, reinterpret_cast<const void*>(throw_info + 12), 4);
  const uint64_t cta = base + static_cast<uint32_t>(cta_rva);
  if (!readable_guest(cta, 8)) return {};
  int32_t first_rva;
  std::memcpy(&first_rva, reinterpret_cast<const void*>(cta + 4), 4);
  const uint64_t catchable = base + static_cast<uint32_t>(first_rva);
  if (!readable_guest(catchable, 8)) return {};
  int32_t type_rva;
  std::memcpy(&type_rva, reinterpret_cast<const void*>(catchable + 4), 4);
  const uint64_t type_desc = base + static_cast<uint32_t>(type_rva);
  if (!readable_guest(type_desc + 16, 1)) return {};
  const char* name = reinterpret_cast<const char*>(type_desc + 16);
  return std::string(name, strnlen(name, 256));
}

void report_unhandled(const ExceptionRecord* rec, const Context* ctx) {
  std::string detail;
  if (rec->Code == kExceptionMsvcCpp) {
    const std::string type = msvc_exception_type(rec);
    detail = type.empty() ? " (C++ exception)" : " (C++ exception of type " + type + ")";
  } else if (rec->Code == kExceptionGccThrow) {
    detail = " (GCC/MinGW C++ exception)";
  }
  XBEM_ERROR("seh", "unhandled guest exception {:#010x} {}{}, flags {:#x}, raised at {}", rec->Code,
             exception_name(rec->Code), detail, rec->Flags, describe(rec->Address));
  Context c = *ctx;
  DispatcherContext dc{};
  for (int i = 0; i < 24; ++i) {
    XBEM_ERROR("seh", "  frame {:>2}: {}", i, describe(c.Rip));
    if (!unwind_one_frame(kUnwFlagNHandler, dc, c) || c.Rip == 0) break;
  }
}

enum class DispatchResult { ContinueExecution, Unhandled };

DispatchResult dispatch_frames(ExceptionRecord* rec, Context* orig) {
  alignas(16) Context context = *orig;
  // Handlers may pass `orig` to RtlUnwindEx as its scratch context (libgcc
  // does), which overwrites it. Unwinds that cross back into this dispatcher
  // continue from this private copy of the raise point.
  alignas(16) const Context raise_point = *orig;
  DispatcherContext dc{};
  dc.ContextRecord = &context;
  const int index = push_record(ActiveRecord::Dispatch, XBEM_FRAME_ADDRESS(),
                                &raise_point, &dc);
  bool have_frame = false;
  DispatchResult result = DispatchResult::Unhandled;
  for (int n = 0; n < kMaxFrames; ++n) {
    if (!have_frame) {
      const uint64_t before = context.Gpr[kRsp];
      if (!unwind_one_frame(kUnwFlagEHandler, dc, context)) {
        // Host code. Either the bottom of the guest stack (unhandled), or a
        // dispatcher/unwinder that called the handler which raised this.
        const ActiveRecord* owner = owner_of_host_frame(context.Gpr[kRsp]);
        if (!owner || owner == &t_records[index]) break;
        if (owner->kind == ActiveRecord::Unwind) {
          // Raised by an unwind handler: search on from the frame being unwound.
          take_over_frame(dc, *owner->dc);
          context = *owner->dc->ContextRecord;
          dc.ContextRecord = &context;
          redo_unwind(dc, context);
          have_frame = true;
        } else {
          // Raised by an exception handler: skip the frames the outer search covered.
          context = *owner->dc->ContextRecord;
          dc.ContextRecord = &context;
          rec->Flags |= kExceptionNestedCall;
          continue;
        }
      } else if (context.Gpr[kRsp] <= before &&
                 dc.ControlPc != reinterpret_cast<uint64_t>(&xbem_seh_consolidate_return)) {
        rec->Flags |= kExceptionStackInvalid;
        break;
      }
    }
    have_frame = false;
    if (!dc.EstablisherFrame) break;
    if (!valid_frame(dc.EstablisherFrame)) {
      XBEM_WARN("seh", "invalid frame {:#x} while dispatching", dc.EstablisherFrame);
      rec->Flags |= kExceptionStackInvalid;
      break;
    }
    if (!dc.LanguageHandler) continue;

    XBEM_TRACE("seh", "dispatch {:#x}: handler {} for frame {:#x}", rec->Code,
               describe(reinterpret_cast<uint64_t>(dc.LanguageHandler)), dc.EstablisherFrame);
    const int32_t res =
        reinterpret_cast<LanguageHandler>(dc.LanguageHandler)(rec, dc.EstablisherFrame, orig, &dc);
    rec->Flags &= kExceptionNoncontinuable;
    if (res == kContinueExecution) {
      if (rec->Flags & kExceptionNoncontinuable) fatal("handler continued a non-continuable exception",
                                                       kStatusNoncontinuableException);
      result = DispatchResult::ContinueExecution;
      break;
    }
    if (res == kContinueSearch) continue;
    if (res == kNestedException) {
      XBEM_WARN("seh", "handler reported a nested exception; continuing the search");
      continue;
    }
    if (res == kCollidedUnwind) {
      context = *dc.ContextRecord;
      dc.ContextRecord = &context;
      redo_unwind(dc, context);
      have_frame = true;
      continue;
    }
    fatal("exception handler returned an invalid disposition", kStatusInvalidDisposition);
  }
  pop_records(index);
  return result;
}

}  // namespace

// ================================================================ public API

const char* exception_name(uint32_t code) {
  switch (code) {
    case kStatusAccessViolation: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC000001D: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC00000FD: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail fast / security check)";
    case 0x80000003: return "EXCEPTION_BREAKPOINT";
    case kExceptionMsvcCpp: return "MSVC C++ exception";
    case kExceptionGccThrow: return "GCC C++ exception";
    case kStatusNoncontinuableException: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case 0xC0000017: return "STATUS_NO_MEMORY";
    default: return "";
  }
}

const RuntimeFunction* lookup_function_entry(uint64_t pc, uint64_t* image_base) {
  if (rt::Process* p = rt::Process::current_or_null()) {
    if (rt::LoadedModule* m = p->loader().find_containing(pc); m && m->image) {
      const auto& dir = m->image->directory(pe::kDirException);
      if (!dir.VirtualAddress || !dir.Size) return nullptr;
      const auto* table = reinterpret_cast<const RuntimeFunction*>(m->base + dir.VirtualAddress);
      const RuntimeFunction* fn = search_table(table, dir.Size / sizeof(RuntimeFunction),
                                               static_cast<uint32_t>(pc - m->handle()));
      if (!fn) return nullptr;
      // Indirect entry: UnwindData points at another RUNTIME_FUNCTION (low bit set).
      if (fn->UnwindData & 1) fn = reinterpret_cast<const RuntimeFunction*>(m->base + (fn->UnwindData & ~1u));
      if (image_base) *image_base = m->handle();
      return fn;
    }
  }
  std::lock_guard lock(g_tables_mutex);
  for (const DynamicTable& t : g_dynamic_tables) {
    if (pc < t.base) continue;
    if (const RuntimeFunction* fn = search_table(t.table, t.count, static_cast<uint32_t>(pc - t.base))) {
      if (image_base) *image_base = t.base;
      return fn;
    }
  }
  return nullptr;
}

bool is_guest_code(uint64_t pc) {
  if (rt::Process* p = rt::Process::current_or_null()) {
    if (rt::LoadedModule* m = p->loader().find_containing(pc)) return !m->hle;
  }
  uint64_t base;
  return lookup_function_entry(pc, &base) != nullptr;
}

void* virtual_unwind(uint32_t type, uint64_t base, uint64_t pc, const RuntimeFunction* fn, Context* ctx,
                     void** handler_data, uint64_t* frame_ret, ContextPointers* ptrs) {
  Context& c = *ctx;
  uint64_t frame = c.Gpr[kRsp];
  *frame_ret = frame;
  bool mach_frame = false;
  const uint8_t* info = nullptr;
  uint32_t prolog_offset = ~0u;

  for (int chain = 0; chain < 32; ++chain) {
    info = reinterpret_cast<const uint8_t*>(base + fn->UnwindData);
    const uint8_t version = info[0] & 7;
    const uint8_t flags = info[0] >> 3;
    const uint8_t prolog = info[1];
    const uint8_t count = info[2];
    const uint8_t frame_reg = info[3] & 0xF;
    const uint8_t frame_off = info[3] >> 4;
    const uint8_t* codes = info + 4;

    if (version != 1 && version != 2) {
      XBEM_WARN("seh", "unknown unwind info version {} at {}", version, describe(pc));
      return nullptr;
    }
    if (frame_reg) frame = c.Gpr[frame_reg] - frame_off * 16u;

    if (pc >= base + fn->BeginAddress && pc < base + fn->BeginAddress + prolog) {
      prolog_offset = static_cast<uint32_t>(pc - base - fn->BeginAddress);
    } else {
      prolog_offset = ~0u;
      if (count && is_inside_epilog(reinterpret_cast<const uint8_t*>(pc), base, fn)) {
        interpret_epilog(reinterpret_cast<const uint8_t*>(pc), c, ptrs);
        *frame_ret = frame;
        return nullptr;
      }
    }

    for (unsigned i = 0; i < count;) {
      const uint8_t code_offset = codes[i * 2];
      const uint8_t op = codes[i * 2 + 1] & 0xF;
      const uint8_t op_info = codes[i * 2 + 1] >> 4;
      const int slots = opcode_slots(op, op_info);
      auto slot16 = [&](unsigned k) {
        uint16_t v;
        std::memcpy(&v, codes + (i + k) * 2, 2);
        return v;
      };
      auto slot32 = [&](unsigned k) {
        uint32_t v;
        std::memcpy(&v, codes + (i + k) * 2, 4);
        return v;
      };
      if (prolog_offset >= code_offset) {  // this prolog instruction has executed
        switch (op) {
          case 0:  // UWOP_PUSH_NONVOL
            set_int_reg(c, ptrs, op_info, reinterpret_cast<uint64_t*>(c.Gpr[kRsp]));
            c.Gpr[kRsp] += 8;
            break;
          case 1:  // UWOP_ALLOC_LARGE
            c.Gpr[kRsp] += op_info ? slot32(1) : slot16(1) * 8u;
            break;
          case 2:  // UWOP_ALLOC_SMALL
            c.Gpr[kRsp] += (op_info + 1u) * 8u;
            break;
          case 3:  // UWOP_SET_FPREG
            c.Gpr[kRsp] = frame;
            *frame_ret = frame;
            break;
          case 4:  // UWOP_SAVE_NONVOL
            set_int_reg(c, ptrs, op_info, reinterpret_cast<uint64_t*>(frame + slot16(1) * 8u));
            break;
          case 5:  // UWOP_SAVE_NONVOL_FAR
            set_int_reg(c, ptrs, op_info, reinterpret_cast<uint64_t*>(frame + slot32(1)));
            break;
          case 8:  // UWOP_SAVE_XMM128
            set_xmm_reg(c, ptrs, op_info, reinterpret_cast<M128*>(frame + slot16(1) * 16u));
            break;
          case 9:  // UWOP_SAVE_XMM128_FAR
            set_xmm_reg(c, ptrs, op_info, reinterpret_cast<M128*>(frame + slot32(1)));
            break;
          case 10:  // UWOP_PUSH_MACHFRAME
            if (op_info) c.Gpr[kRsp] += 8;  // error code
            c.Rip = *reinterpret_cast<uint64_t*>(c.Gpr[kRsp]);
            c.Gpr[kRsp] = *reinterpret_cast<uint64_t*>(c.Gpr[kRsp] + 24);
            mach_frame = true;
            break;
          case 6:  // UWOP_EPILOG (v2): only describes epilogs
            break;
          default:
            XBEM_WARN("seh", "unsupported unwind op {} at {}", op, describe(pc));
            break;
        }
      }
      i += static_cast<unsigned>(slots);
    }

    if (!(flags & kUnwFlagChainInfo)) break;
    fn = reinterpret_cast<const RuntimeFunction*>(codes + ((count + 1u) & ~1u) * 2);
  }

  if (!mach_frame) {
    c.Rip = *reinterpret_cast<uint64_t*>(c.Gpr[kRsp]);
    c.Gpr[kRsp] += 8;
  }

  const uint8_t flags = info[0] >> 3;
  if (!(flags & type) || prolog_offset != ~0u) return nullptr;
  const uint8_t count = info[2];
  const uint8_t* handler_slot = info + 4 + ((count + 1u) & ~1u) * 2;
  uint32_t handler_rva;
  std::memcpy(&handler_rva, handler_slot, 4);
  if (handler_data) *handler_data = const_cast<uint8_t*>(handler_slot + 4);
  return reinterpret_cast<void*>(base + handler_rva);
}

bool unwind_one_frame(uint32_t type, DispatcherContext& dc, Context& c) {
  dc.ImageBase = 0;
  dc.ScopeIndex = 0;
  dc.ControlPc = c.Rip;
  dc.FunctionEntry = nullptr;
  dc.LanguageHandler = nullptr;
  dc.HandlerData = nullptr;
  const uint64_t pc = c.Rip;

  if (pc == reinterpret_cast<uint64_t>(&xbem_seh_consolidate_return)) {
    // Our consolidation trampoline: continue in the frame the catch block belongs to.
    const uint64_t rsp = c.Gpr[kRsp];
    dc.EstablisherFrame = rsp;
    std::memcpy(&c, reinterpret_cast<const void*>(rsp + kConsolidateContextOffset), sizeof(Context));
    return true;
  }
  uint64_t base = 0;
  if (const RuntimeFunction* fn = lookup_function_entry(pc, &base)) {
    dc.ImageBase = base;
    dc.FunctionEntry = fn;
    dc.LanguageHandler = virtual_unwind(type, base, pc, fn, &c, &dc.HandlerData, &dc.EstablisherFrame, nullptr);
    return true;
  }
  if (is_guest_code(pc)) {
    // Guest code without unwind data is a leaf function: [rsp] is the return address.
    dc.EstablisherFrame = c.Gpr[kRsp];
    c.Rip = *reinterpret_cast<uint64_t*>(c.Gpr[kRsp]);
    c.Gpr[kRsp] += 8;
    return true;
  }
  return false;
}

void raise_exception(ExceptionRecord* rec, Context* ctx) {
  XBEM_DEBUG("seh", "RaiseException {:#010x} {} at {}", rec->Code, exception_name(rec->Code), describe(rec->Address));

  // Conventions a debugger would consume; without one they are no-ops.
  if (rec->Code == 0x406D1388 || rec->Code == 0x40010006 || rec->Code == 0x4001000A) {
    if (rec->Code == 0x40010006 && rec->NumberParameters >= 2 && rec->Information[1])
      log::guest_output(reinterpret_cast<const char*>(rec->Information[1]));
    restore_context(ctx, nullptr);
  }

  if (call_vectored(g_vectored, rec, ctx) == -1) restore_context(ctx, nullptr);

  if (dispatch_frames(rec, ctx) == DispatchResult::ContinueExecution) {
    call_vectored(g_continue, rec, ctx);
    restore_context(ctx, nullptr);
  }

  if (const uint64_t filter = g_unhandled_filter) {
    ExceptionPointers ptrs{rec, ctx};
    const int32_t r = reinterpret_cast<VectoredHandler>(filter)(&ptrs);
    if (r == -1) restore_context(ctx, nullptr);  // EXCEPTION_CONTINUE_EXECUTION
  }
  report_unhandled(rec, ctx);
  rt::Process::current().exit_process(rec->Code);
}

void unwind_ex(uint64_t end_frame, uint64_t target_ip, ExceptionRecord* rec, uint64_t retval, Context* scratch,
               const Context& caller) {
  alignas(16) Context local_context;
  Context* context = scratch ? scratch : &local_context;
  *context = caller;
  alignas(16) Context new_context = *context;

  ExceptionRecord local_rec{};
  if (!rec) {
    local_rec.Code = kStatusUnwind;
    local_rec.Address = caller.Rip;
    rec = &local_rec;
  }
  rec->Flags |= kExceptionUnwinding | (end_frame ? 0 : kExceptionExitUnwind);
  XBEM_DEBUG("seh", "RtlUnwindEx to frame {:#x}, target {}, code {:#x}", end_frame, describe(target_ip), rec->Code);

  DispatcherContext dc{};
  dc.EstablisherFrame = context->Gpr[kRsp];
  dc.TargetIp = target_ip;
  dc.ContextRecord = context;
  const int index = push_record(ActiveRecord::Unwind, XBEM_FRAME_ADDRESS(), nullptr,
                                &dc);

  bool have_frame = false;
  for (int n = 0;; ++n) {
    if (n > kMaxFrames) fatal("RtlUnwindEx: stack walk does not terminate", kStatusInvalidDisposition);
    if (!have_frame && !unwind_one_frame(kUnwFlagUHandler, dc, new_context)) {
      // Host code: the frames we came through belong to a handler invocation.
      const ActiveRecord* owner = owner_of_host_frame(new_context.Gpr[kRsp]);
      if (!owner || owner == &t_records[index])
        fatal("RtlUnwindEx: unwind target not found on the guest stack", kStatusInvalidDisposition);
      if (owner->kind == ActiveRecord::Dispatch) {
        // Called from an exception handler: continue from the raise point.
        *context = *owner->raise_point;
        new_context = *context;
        continue;
      }
      // Called from an unwind handler: a collided unwind. Take over the frame
      // the outer unwind is processing; frames below it are already unwound.
      take_over_frame(dc, *owner->dc);
      *context = *owner->dc->ContextRecord;
      new_context = *context;
      dc.ContextRecord = context;
      redo_unwind(dc, new_context);
      rec->Flags |= kExceptionCollidedUnwind;
    }
    have_frame = false;
    XBEM_TRACE("seh", "unwind step: pc {} frame {:#x}", describe(dc.ControlPc), dc.EstablisherFrame);

    if (!dc.EstablisherFrame) break;
    if (!valid_frame(dc.EstablisherFrame)) fatal("RtlUnwindEx: invalid frame on the stack", kStatusInvalidDisposition);

    if (dc.LanguageHandler) {
      if (end_frame && dc.EstablisherFrame > end_frame)
        fatal("RtlUnwindEx: walked past the target frame", kStatusInvalidDisposition);
      if (dc.EstablisherFrame == end_frame) rec->Flags |= kExceptionTargetUnwind;
      XBEM_TRACE("seh", "unwind: handler {} for frame {:#x}", describe(reinterpret_cast<uint64_t>(dc.LanguageHandler)),
                 dc.EstablisherFrame);
      const int32_t res =
          reinterpret_cast<LanguageHandler>(dc.LanguageHandler)(rec, dc.EstablisherFrame, dc.ContextRecord, &dc);
      if (res == kCollidedUnwind) {
        new_context = *dc.ContextRecord;
        new_context.ContextFlags &= ~0x40u;
        *context = new_context;
        dc.ContextRecord = context;
        redo_unwind(dc, new_context);
        rec->Flags |= kExceptionCollidedUnwind;
        have_frame = true;
        continue;
      }
      rec->Flags &= ~kExceptionCollidedUnwind;
    }
    if (dc.EstablisherFrame == end_frame) break;
    *context = new_context;
  }

  pop_records(index);
  if (rec->Code != kStatusUnwindConsolidate) context->Gpr[kRax] = retval;
  context->Rip = target_ip;
  restore_context(context, rec);
}

void restore_context(Context* ctx, ExceptionRecord* rec) {
  if (rec && rec->Code == kStatusLongjump && rec->NumberParameters >= 1) {
    // _JUMP_BUFFER: Frame Rbx Rsp Rbp Rsi Rdi R12 R13 R14 R15 Rip MxCsr FpCsr Spare Xmm6..Xmm15
    const auto* jb = reinterpret_cast<const uint64_t*>(rec->Information[0]);
    ctx->Gpr[kRbx] = jb[1];
    ctx->Gpr[kRsp] = jb[2];
    ctx->Gpr[kRbp] = jb[3];
    ctx->Gpr[kRsi] = jb[4];
    ctx->Gpr[kRdi] = jb[5];
    ctx->Gpr[kR12] = jb[6];
    ctx->Gpr[kR13] = jb[7];
    ctx->Gpr[kR14] = jb[8];
    ctx->Gpr[kR15] = jb[9];
    ctx->Rip = jb[10];
    std::memcpy(&ctx->MxCsr, &jb[11], 4);
    std::memcpy(&ctx->Xmm[6], &jb[12], 10 * sizeof(M128));
  } else if (rec && rec->Code == kStatusUnwindConsolidate && rec->NumberParameters >= 1) {
    // MSVC: Information[0] is __CxxCallCatchBlock. It runs the catch block and
    // returns where execution continues. [10] lets it adjust the target RSP.
    rec->Information[10] = reinterpret_cast<uint64_t>(&ctx->Gpr[kRsp]);
    XBEM_TRACE("seh", "consolidate callback {}", describe(rec->Information[0]));
    ctx->Rip = xbem_seh_call_consolidate(rec->Information[0], rec, ctx);
  }
  purge_records_below(ctx->Gpr[kRsp]);
  if (ctx->MxCsr == 0) ctx->MxCsr = 0x1F80;  // never unmask every SSE exception by accident
  xbem_seh_restore_context_asm(ctx);
}

uint16_t capture_back_trace(const Context& from, uint32_t skip, uint32_t count, uint64_t* out, uint32_t* hash) {
  alignas(16) Context c = from;
  DispatcherContext dc{};
  uint32_t n = 0, index = 0, sum = 0;
  while (n < count && index < 1024 && c.Rip) {
    if (index >= skip) {
      out[n++] = c.Rip;
      sum += static_cast<uint32_t>(c.Rip);
    }
    ++index;
    if (!unwind_one_frame(kUnwFlagNHandler, dc, c)) break;
  }
  if (hash) *hash = sum;
  return static_cast<uint16_t>(n);
}

bool add_function_table(const RuntimeFunction* table, uint32_t count, uint64_t base) {
  if (!table) return false;
  std::lock_guard lock(g_tables_mutex);
  g_dynamic_tables.push_back({table, count, base});
  return true;
}

bool delete_function_table(const RuntimeFunction* table) {
  std::lock_guard lock(g_tables_mutex);
  const auto it = std::find_if(g_dynamic_tables.begin(), g_dynamic_tables.end(),
                               [&](const DynamicTable& t) { return t.table == table; });
  if (it == g_dynamic_tables.end()) return false;
  g_dynamic_tables.erase(it);
  return true;
}

uint64_t add_vectored_handler(bool first, uint64_t handler, bool continue_handler) {
  std::lock_guard lock(g_veh_mutex);
  auto& list = continue_handler ? g_continue : g_vectored;
  const VectoredEntry e{g_next_veh_id++, handler};
  if (first) list.insert(list.begin(), e);
  else list.push_back(e);
  return e.id;
}

bool remove_vectored_handler(uint64_t handle, bool continue_handler) {
  std::lock_guard lock(g_veh_mutex);
  auto& list = continue_handler ? g_continue : g_vectored;
  const auto it = std::find_if(list.begin(), list.end(), [&](const VectoredEntry& e) { return e.id == handle; });
  if (it == list.end()) return false;
  list.erase(it);
  return true;
}

uint64_t set_unhandled_exception_filter(uint64_t filter) {
  const uint64_t old = g_unhandled_filter;
  g_unhandled_filter = filter;
  return old;
}

}  // namespace xbem::seh

// ====================================================== entry from assembly

using xbem::seh::Context;
using xbem::seh::ExceptionRecord;

extern "C" {

[[noreturn]] void xbem_seh_raise_exception_impl(uint64_t code, uint64_t flags, uint64_t nargs, uint64_t args,
                                                Context* ctx, uint64_t*) {
  ExceptionRecord rec{};
  rec.Code = static_cast<uint32_t>(code);
  rec.Flags = static_cast<uint32_t>(flags) & xbem::seh::kExceptionNoncontinuable;
  rec.Address = ctx->Rip;
  rec.NumberParameters = std::min<uint32_t>(static_cast<uint32_t>(nargs), 15);
  if (args && rec.NumberParameters)
    std::memcpy(rec.Information, reinterpret_cast<const void*>(args), rec.NumberParameters * 8);
  else
    rec.NumberParameters = 0;
  xbem::seh::raise_exception(&rec, ctx);
}

[[noreturn]] void xbem_seh_raise_record_impl(uint64_t rec, uint64_t, uint64_t, uint64_t, Context* ctx, uint64_t*) {
  auto* r = reinterpret_cast<ExceptionRecord*>(rec);
  r->Address = ctx->Rip;
  xbem::seh::raise_exception(r, ctx);
}

[[noreturn]] void xbem_seh_unwind_ex_impl(uint64_t end_frame, uint64_t target_ip, uint64_t rec, uint64_t retval,
                                          Context* caller, uint64_t* stack_args) {
  // RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, ContextRecord, HistoryTable)
  xbem::seh::unwind_ex(end_frame, target_ip, reinterpret_cast<ExceptionRecord*>(rec), retval,
                       reinterpret_cast<Context*>(stack_args[0]), *caller);
}

[[noreturn]] void xbem_seh_unwind_impl(uint64_t end_frame, uint64_t target_ip, uint64_t rec, uint64_t retval,
                                       Context* caller, uint64_t*) {
  xbem::seh::unwind_ex(end_frame, target_ip, reinterpret_cast<ExceptionRecord*>(rec), retval, nullptr, *caller);
}

uint64_t xbem_seh_backtrace_impl(uint64_t skip, uint64_t count, uint64_t out, uint64_t hash, Context* caller,
                                 uint64_t*) {
  if (!out) return 0;
  return xbem::seh::capture_back_trace(*caller, static_cast<uint32_t>(skip), static_cast<uint32_t>(count),
                                       reinterpret_cast<uint64_t*>(out), reinterpret_cast<uint32_t*>(hash));
}

}  // extern "C"
