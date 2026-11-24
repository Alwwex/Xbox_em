#pragma once

#include <cstddef>
#include <cstdint>

namespace xbem::platform {

// Formats a code/data address for crash reports, e.g. "game.exe+0x1234".
// Called from a signal handler: must not allocate or take locks if avoidable.
using AddressDescriber = void (*)(uint64_t address, char* out, size_t out_size);

// Installs process-wide handlers for SIGSEGV/SIGILL/SIGFPE/SIGBUS that print a
// guest-oriented crash report (registers, module+offset) and exit.
void install_crash_handler(AddressDescriber describer);

// Each thread needs its own alternate signal stack so that a stack overflow in
// guest code can still be reported.
void setup_thread_signal_stack();

// Describes what the call instruction before a return address calls, e.g.
// "calls kernelx.dll!WaitForSingleObject" (empty if unknown). Signal-safe.
using CallSiteDescriber = void (*)(uint64_t return_address, char* out, size_t out_size);

// Thread dump for hangs: Ctrl+C (SIGINT) prints where every registered guest
// thread is - its instruction pointer and the guest return addresses on its
// stack, with the imported function each call site calls - and exits.
// Ctrl+\ (SIGQUIT) prints the same dump and lets the title continue.
void install_thread_dump(AddressDescriber describer, CallSiteDescriber call_site);
void register_dump_thread(uint32_t guest_thread_id, uint64_t stack_low, uint64_t stack_high);

// Crash reports also say which allocation a fault address / stack pointer is in.
void set_memory_describer(AddressDescriber describer);

// int3 / __debugbreak() in guest code normally ends the run with a crash
// report; when enabled, it is logged and execution continues.
void set_continue_after_breakpoint(bool enabled);

// Console-CPU instructions the host may lack are handled when they fault
// (SIGILL). Cache write-back hints (CLWB, CLFLUSHOPT) are rewritten in place
// into a same-length NOP; returns the instruction's name, or nullptr when the
// code is not one of them. Exposed for tests.
const char* patch_cache_hint_instruction(uint8_t* code);

// Prints extra per-thread context (e.g. the thread's recent emulated calls)
// into crash reports and thread dumps, using the given writer.
using ThreadContextPrinter = void (*)(void (*write)(const char*));
void set_thread_context_printer(ThreadContextPrinter printer);

// Prints up to `max_frames` frames of a backtrace from a register state using
// the guest's unwind data (.pdata), with each frame's function start. `gpr` is
// in unwind-code order (RAX RCX RDX RBX RSP RBP RSI RDI R8..R15); the stack
// bounds limit every frame the walk may read.
using BacktracePrinter = void (*)(const uint64_t gpr[16], uint64_t rip, uint64_t stack_low, uint64_t stack_high,
                                  int max_frames, void (*write)(const char*));
void set_backtrace_printer(BacktracePrinter printer);

// Code tracepoints (run --trace-at). An int3 replaces the first byte of a
// guest instruction; a hit records the registers and [rsp] (crash reports list
// the last hits of all tracepoints in order), the first hits of each are
// logged with a short backtrace, then the instruction is single-stepped and
// the int3 put back. A thread reaching a tracepoint while another one steps
// over it is not counted. `label` names it in logs. Returns false when the
// code cannot be patched (or the table is full).
bool add_tracepoint(uint64_t address, const char* label);
void set_tracepoint_log_hits(int hits);
void unregister_dump_thread();

}  // namespace xbem::platform
