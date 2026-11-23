// Machine-code side of SEH: context capture/restore and the consolidation
// trampoline. Offsets are those of xbem::seh::Context (checked in seh.cpp):
//   0x30 ContextFlags  0x34 MxCsr  0x38 SegCs  0x42 SegSs  0x44 EFlags
//   0x78 + 8*n  Gpr[n] (RAX RCX RDX RBX RSP RBP RSI RDI R8..R15)   0xF8 Rip
//   0x100 FltSave.ControlWord  0x118 FltSave.MxCsr  0x1A0 + 16*n  Xmm[n]

#include <cstdint>
#include <cstdlib>

#include "runtime/seh.h"

#if defined(__x86_64__) && !defined(_WIN32)

// Called from the thunks with the System V ABI:
//   impl(rcx, rdx, r8, r9 of the guest call, Context* caller, uint64_t* guest_stack_args)
extern "C" {
[[noreturn]] void xbem_seh_raise_exception_impl(uint64_t, uint64_t, uint64_t, uint64_t, xbem::seh::Context*,
                                                uint64_t*);
[[noreturn]] void xbem_seh_raise_record_impl(uint64_t, uint64_t, uint64_t, uint64_t, xbem::seh::Context*, uint64_t*);
[[noreturn]] void xbem_seh_unwind_ex_impl(uint64_t, uint64_t, uint64_t, uint64_t, xbem::seh::Context*, uint64_t*);
[[noreturn]] void xbem_seh_unwind_impl(uint64_t, uint64_t, uint64_t, uint64_t, xbem::seh::Context*, uint64_t*);
uint64_t xbem_seh_backtrace_impl(uint64_t, uint64_t, uint64_t, uint64_t, xbem::seh::Context*, uint64_t*);
}

__asm__(R"ASM(
    .intel_syntax noprefix
    .text

# Stores the caller's state into a CONTEXT at [rsp] (0x4D0 bytes reserved
# by the thunk, return address at [rsp+0x4D8]) as if the call had returned.
.macro XBEM_STORE_CALLER_CONTEXT
    mov [rsp+0x78], rax
    mov [rsp+0x80], rcx
    mov [rsp+0x88], rdx
    mov [rsp+0x90], rbx
    mov [rsp+0xa0], rbp
    mov [rsp+0xa8], rsi
    mov [rsp+0xb0], rdi
    mov [rsp+0xb8], r8
    mov [rsp+0xc0], r9
    mov [rsp+0xc8], r10
    mov [rsp+0xd0], r11
    mov [rsp+0xd8], r12
    mov [rsp+0xe0], r13
    mov [rsp+0xe8], r14
    mov [rsp+0xf0], r15
    lea rax, [rsp+0x4e0]
    mov [rsp+0x98], rax
    mov rax, [rsp+0x4d8]
    mov [rsp+0xf8], rax
    pushfq
    pop rax
    mov [rsp+0x44], eax
    stmxcsr dword ptr [rsp+0x34]
    stmxcsr dword ptr [rsp+0x118]
    fnstcw word ptr [rsp+0x100]
    movdqa [rsp+0x1a0], xmm0
    movdqa [rsp+0x1b0], xmm1
    movdqa [rsp+0x1c0], xmm2
    movdqa [rsp+0x1d0], xmm3
    movdqa [rsp+0x1e0], xmm4
    movdqa [rsp+0x1f0], xmm5
    movdqa [rsp+0x200], xmm6
    movdqa [rsp+0x210], xmm7
    movdqa [rsp+0x220], xmm8
    movdqa [rsp+0x230], xmm9
    movdqa [rsp+0x240], xmm10
    movdqa [rsp+0x250], xmm11
    movdqa [rsp+0x260], xmm12
    movdqa [rsp+0x270], xmm13
    movdqa [rsp+0x280], xmm14
    movdqa [rsp+0x290], xmm15
    mov dword ptr [rsp+0x30], 0x10000b
    mov word ptr [rsp+0x38], cs
    mov word ptr [rsp+0x42], ss
.endm

# Guest-callable thunk (Microsoft x64 ABI) that captures the caller context
# and calls impl(rcx, rdx, r8, r9, &context, &stack_args) (System V ABI).
.macro XBEM_CAPTURE_THUNK name, impl, returns
    .globl \name
    .type \name, @function
    .p2align 4
\name:
    sub rsp, 0x4d8
    XBEM_STORE_CALLER_CONTEXT
    mov rdi, rcx
    mov rsi, rdx
    mov rdx, r8
    mov rcx, r9
    mov r8, rsp
    lea r9, [rsp+0x500]
    call \impl
    .if \returns
    # Restore registers that are callee-saved in the Microsoft ABI but not in System V.
    mov rdi, [rsp+0xb0]
    mov rsi, [rsp+0xa8]
    movdqa xmm6, [rsp+0x200]
    movdqa xmm7, [rsp+0x210]
    movdqa xmm8, [rsp+0x220]
    movdqa xmm9, [rsp+0x230]
    movdqa xmm10, [rsp+0x240]
    movdqa xmm11, [rsp+0x250]
    movdqa xmm12, [rsp+0x260]
    movdqa xmm13, [rsp+0x270]
    movdqa xmm14, [rsp+0x280]
    movdqa xmm15, [rsp+0x290]
    add rsp, 0x4d8
    ret
    .else
    ud2
    .endif
    .size \name, .-\name
.endm

    XBEM_CAPTURE_THUNK xbem_seh_RaiseException, xbem_seh_raise_exception_impl, 0
    XBEM_CAPTURE_THUNK xbem_seh_RtlRaiseException, xbem_seh_raise_record_impl, 0
    XBEM_CAPTURE_THUNK xbem_seh_RtlUnwindEx, xbem_seh_unwind_ex_impl, 0
    XBEM_CAPTURE_THUNK xbem_seh_RtlUnwind, xbem_seh_unwind_impl, 0
    XBEM_CAPTURE_THUNK xbem_seh_RtlCaptureStackBackTrace, xbem_seh_backtrace_impl, 1

# void RtlCaptureContext(CONTEXT* rcx)  (Microsoft x64 ABI)
    .globl xbem_seh_RtlCaptureContext
    .type xbem_seh_RtlCaptureContext, @function
    .p2align 4
xbem_seh_RtlCaptureContext:
    mov [rcx+0x78], rax
    mov [rcx+0x80], rcx
    mov [rcx+0x88], rdx
    mov [rcx+0x90], rbx
    mov [rcx+0xa0], rbp
    mov [rcx+0xa8], rsi
    mov [rcx+0xb0], rdi
    mov [rcx+0xb8], r8
    mov [rcx+0xc0], r9
    mov [rcx+0xc8], r10
    mov [rcx+0xd0], r11
    mov [rcx+0xd8], r12
    mov [rcx+0xe0], r13
    mov [rcx+0xe8], r14
    mov [rcx+0xf0], r15
    lea rax, [rsp+8]
    mov [rcx+0x98], rax
    mov rax, [rsp]
    mov [rcx+0xf8], rax
    pushfq
    pop rax
    mov [rcx+0x44], eax
    stmxcsr dword ptr [rcx+0x34]
    stmxcsr dword ptr [rcx+0x118]
    fnstcw word ptr [rcx+0x100]
    movdqu [rcx+0x1a0], xmm0
    movdqu [rcx+0x1b0], xmm1
    movdqu [rcx+0x1c0], xmm2
    movdqu [rcx+0x1d0], xmm3
    movdqu [rcx+0x1e0], xmm4
    movdqu [rcx+0x1f0], xmm5
    movdqu [rcx+0x200], xmm6
    movdqu [rcx+0x210], xmm7
    movdqu [rcx+0x220], xmm8
    movdqu [rcx+0x230], xmm9
    movdqu [rcx+0x240], xmm10
    movdqu [rcx+0x250], xmm11
    movdqu [rcx+0x260], xmm12
    movdqu [rcx+0x270], xmm13
    movdqu [rcx+0x280], xmm14
    movdqu [rcx+0x290], xmm15
    mov dword ptr [rcx+0x30], 0x10000b
    mov word ptr [rcx+0x38], cs
    mov word ptr [rcx+0x42], ss
    mov rax, [rcx+0x78]
    ret
    .size xbem_seh_RtlCaptureContext, .-xbem_seh_RtlCaptureContext

# [[noreturn]] void xbem_seh_restore_context_asm(const CONTEXT* rdi)  (System V)
# Loads every register from the context and jumps to its Rip.
    .globl xbem_seh_restore_context_asm
    .type xbem_seh_restore_context_asm, @function
    .p2align 4
xbem_seh_restore_context_asm:
    cld
    mov rax, rdi
    ldmxcsr dword ptr [rax+0x34]
    movdqu xmm0, [rax+0x1a0]
    movdqu xmm1, [rax+0x1b0]
    movdqu xmm2, [rax+0x1c0]
    movdqu xmm3, [rax+0x1d0]
    movdqu xmm4, [rax+0x1e0]
    movdqu xmm5, [rax+0x1f0]
    movdqu xmm6, [rax+0x200]
    movdqu xmm7, [rax+0x210]
    movdqu xmm8, [rax+0x220]
    movdqu xmm9, [rax+0x230]
    movdqu xmm10, [rax+0x240]
    movdqu xmm11, [rax+0x250]
    movdqu xmm12, [rax+0x260]
    movdqu xmm13, [rax+0x270]
    movdqu xmm14, [rax+0x280]
    movdqu xmm15, [rax+0x290]
    mov rcx, [rax+0x98]
    sub rcx, 8
    mov rdx, [rax+0xf8]
    mov [rcx], rdx
    mov rdx, [rax+0x88]
    mov rbx, [rax+0x90]
    mov rbp, [rax+0xa0]
    mov rsi, [rax+0xa8]
    mov rdi, [rax+0xb0]
    mov r8, [rax+0xb8]
    mov r9, [rax+0xc0]
    mov r10, [rax+0xc8]
    mov r11, [rax+0xd0]
    mov r12, [rax+0xd8]
    mov r13, [rax+0xe0]
    mov r14, [rax+0xe8]
    mov r15, [rax+0xf0]
    mov rsp, rcx
    mov rcx, [rax+0x80]
    mov rax, [rax+0x78]
    ret
    .size xbem_seh_restore_context_asm, .-xbem_seh_restore_context_asm

# uint64_t xbem_seh_call_consolidate(callback rdi, EXCEPTION_RECORD* rsi, const CONTEXT* rdx)  (System V)
# Runs the consolidation callback (MSVC: the catch block) below a copy of the
# target context. The copy lives at [rsp+0x20] of this frame when the callback
# returns to xbem_seh_consolidate_return; the unwinder uses it to unwind
# through this frame if the catch block raises again.
    .globl xbem_seh_call_consolidate
    .type xbem_seh_call_consolidate, @function
    .p2align 4
xbem_seh_call_consolidate:
    push rbp
    mov rbp, rsp
    push rbx
    sub rsp, 0x4f8
    mov rbx, rdi
    mov r8, rsi
    mov rsi, rdx
    lea rdi, [rsp+0x20]
    mov ecx, 0x4d0
    cld
    rep movsb
    mov rcx, r8
    call rbx
    .globl xbem_seh_consolidate_return
xbem_seh_consolidate_return:
    lea rsp, [rbp-8]
    pop rbx
    pop rbp
    ret
    .size xbem_seh_call_consolidate, .-xbem_seh_call_consolidate

    .att_syntax prefix
)ASM");

#else  // Hosts that cannot execute guest code: placeholders, never called.

extern "C" {
void xbem_seh_RaiseException() { std::abort(); }
void xbem_seh_RtlRaiseException() { std::abort(); }
void xbem_seh_RtlCaptureContext() { std::abort(); }
void xbem_seh_RtlUnwindEx() { std::abort(); }
void xbem_seh_RtlUnwind() { std::abort(); }
void xbem_seh_RtlCaptureStackBackTrace() { std::abort(); }
void xbem_seh_consolidate_return() { std::abort(); }
void xbem_seh_restore_context_asm(const xbem::seh::Context*) { std::abort(); }
uint64_t xbem_seh_call_consolidate(uint64_t, xbem::seh::ExceptionRecord*, const xbem::seh::Context*) {
  std::abort();
}
}

#endif
