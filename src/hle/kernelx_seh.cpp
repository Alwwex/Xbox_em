// kernelx / ntdll-level exception handling exports. The machinery lives in
// runtime/seh.*; functions that need the caller's exact register state are
// assembly thunks (runtime/seh_asm.cpp).

#include "hle/hle_util.h"
#include "runtime/seh.h"

namespace xbem::hle {
namespace {

using namespace win;
using seh::Context;
using seh::ExceptionRecord;
using seh::RuntimeFunction;

void* XBEM_GUEST_ABI hle_RtlLookupFunctionEntry(uint64_t pc, uint64_t* image_base, void*) {
  uint64_t base = 0;
  const RuntimeFunction* fn = seh::lookup_function_entry(pc, &base);
  if (fn && image_base) *image_base = base;
  return const_cast<RuntimeFunction*>(fn);
}

void* XBEM_GUEST_ABI hle_RtlVirtualUnwind(DWORD type, uint64_t image_base, uint64_t pc, const RuntimeFunction* fn,
                                          Context* ctx, void** handler_data, uint64_t* frame,
                                          seh::ContextPointers* pointers) {
  void* dummy_data = nullptr;
  uint64_t dummy_frame = 0;
  return seh::virtual_unwind(type, image_base, pc, fn, ctx, handler_data ? handler_data : &dummy_data,
                             frame ? frame : &dummy_frame, pointers);
}

void* XBEM_GUEST_ABI hle_RtlPcToFileHeader(uint64_t pc, uint64_t* base_of_image) {
  rt::LoadedModule* m = proc().loader().find_containing(pc);
  const uint64_t base = m ? m->handle() : 0;
  if (base_of_image) *base_of_image = base;
  return reinterpret_cast<void*>(base);
}

[[noreturn]] void XBEM_GUEST_ABI hle_RtlRestoreContext(Context* ctx, ExceptionRecord* rec) {
  seh::restore_context(ctx, rec);
}

BOOLEAN XBEM_GUEST_ABI hle_RtlAddFunctionTable(const RuntimeFunction* table, DWORD count, uint64_t base) {
  return seh::add_function_table(table, count, base) ? 1 : 0;
}

BOOLEAN XBEM_GUEST_ABI hle_RtlDeleteFunctionTable(const RuntimeFunction* table) {
  return seh::delete_function_table(table) ? 1 : 0;
}

void* XBEM_GUEST_ABI hle_AddVectoredExceptionHandler(ULONG first, uint64_t handler) {
  return reinterpret_cast<void*>(seh::add_vectored_handler(first != 0, handler, false));
}

ULONG XBEM_GUEST_ABI hle_RemoveVectoredExceptionHandler(void* handle) {
  return seh::remove_vectored_handler(reinterpret_cast<uint64_t>(handle), false) ? 1 : 0;
}

void* XBEM_GUEST_ABI hle_AddVectoredContinueHandler(ULONG first, uint64_t handler) {
  return reinterpret_cast<void*>(seh::add_vectored_handler(first != 0, handler, true));
}

ULONG XBEM_GUEST_ABI hle_RemoveVectoredContinueHandler(void* handle) {
  return seh::remove_vectored_handler(reinterpret_cast<uint64_t>(handle), true) ? 1 : 0;
}

uint64_t XBEM_GUEST_ABI hle_SetUnhandledExceptionFilter(uint64_t filter) {
  return seh::set_unhandled_exception_filter(filter);
}

LONG XBEM_GUEST_ABI hle_UnhandledExceptionFilter(seh::ExceptionPointers* ptrs) {
  if (ptrs && ptrs->record)
    XBEM_ERROR("kernelx", "UnhandledExceptionFilter: exception {:#x} {}", ptrs->record->Code,
               seh::exception_name(ptrs->record->Code));
  return 1;  // EXCEPTION_EXECUTE_HANDLER: the caller terminates the process
}

}  // namespace

void add_kernelx_seh_exports(std::vector<ExportDef>& out) {
  auto asm_export = [](std::string_view name, void (*fn)()) { return ExportDef{name, reinterpret_cast<void*>(fn)}; };
  out.insert(out.end(), {
      asm_export("RaiseException", &xbem_seh_RaiseException),
      asm_export("RtlRaiseException", &xbem_seh_RtlRaiseException),
      asm_export("RtlCaptureContext", &xbem_seh_RtlCaptureContext),
      asm_export("RtlUnwindEx", &xbem_seh_RtlUnwindEx),
      asm_export("RtlUnwind", &xbem_seh_RtlUnwind),
      asm_export("RtlCaptureStackBackTrace", &xbem_seh_RtlCaptureStackBackTrace),
      XBEM_HLE(RtlLookupFunctionEntry), XBEM_HLE(RtlVirtualUnwind), XBEM_HLE(RtlPcToFileHeader),
      XBEM_HLE(RtlRestoreContext), XBEM_HLE(RtlAddFunctionTable), XBEM_HLE(RtlDeleteFunctionTable),
      XBEM_HLE(AddVectoredExceptionHandler), XBEM_HLE(RemoveVectoredExceptionHandler),
      XBEM_HLE(AddVectoredContinueHandler), XBEM_HLE(RemoveVectoredContinueHandler),
      XBEM_HLE(SetUnhandledExceptionFilter), XBEM_HLE(UnhandledExceptionFilter),
  });
}

}  // namespace xbem::hle
