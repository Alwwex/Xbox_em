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
//
// Options: a condition limits logging and recording to hits where a register
// equals (or differs from) a value; memory ranges are dumped with each logged
// hit, at register + offset or at [register + offset] + offset (up to four
// brackets deep: [[register + offset] + offset] + offset).
struct TraceMemory {
  int reg = -1;           // unwind-code register number (0 RAX .. 15 R15); -1 = unused
  bool indirect = false;  // address = [reg + inner] + outer, otherwise reg + outer
  int64_t inner = 0;
  int64_t outer = 0;
  // Deeper levels, "[[reg + inner] + deeper[0]] + outer" and so on: after
  // the first load, each of `levels` more pointers is loaded at pointer + deeper[i].
  int levels = 0;
  int64_t deeper[3] = {};
  uint32_t length = 64;   // at most 1024
  char text[48] = {};     // as written, for the log
};
struct TracepointOptions {
  int cond_reg = -1;
  bool cond_not_equal = false;
  uint64_t cond_value = 0;
  TraceMemory memory[4];
  // Hits are counted and recorded but never logged (a tracepoint that only
  // arms a watchpoint).
  bool quiet = false;
  // The first hit that meets the condition arms the watchpoint reserved as
  // `arm_watch` (reserve_watchpoint) at the address of `arm_at`; -1 = none.
  int arm_watch = -1;
  TraceMemory arm_at;
};
bool add_tracepoint(uint64_t address, const char* label, const TracepointOptions& options = TracepointOptions{});
// Logs matched hits skip+1 .. skip+hits of each tracepoint as they happen.
void set_tracepoint_log_hits(int hits, uint64_t skip = 0);

// Data watchpoints (run --watch). The page holding [address, address +
// length) becomes read-only; a write into the range is logged (writing
// instruction, bytes before and after, registers, a short backtrace) and
// recorded for crash reports, then the instruction is single-stepped with the
// page writable. Other writes to the page are stepped over silently, so a
// busy page slows the title down. Writes by the kernel (read() into the
// page) would fail instead of faulting: watch data structures, not I/O
// buffers. `length` is 1..64 bytes; the first `log_writes` writes of each
// watchpoint are logged.
bool add_watchpoint(uint64_t address, uint32_t length, const char* label);
// A watchpoint whose address is only known at run time (run --watch
// at:CODE:OPERAND): reserved now, armed by a tracepoint hit (see
// TracepointOptions::arm_watch). Returns its index, -1 when the table is full.
int reserve_watchpoint(uint32_t length, const char* label);
// Memory mapped at several addresses (emulated physical pages): a watchpoint
// also guards the other views of its bytes. The finder lists every address
// the byte at `address` is mapped at, itself included (at most `max`; 0 when
// it is not such memory, -1 when it cannot tell without blocking and
// `may_block` is false). The emulator calls refresh_watchpoint_views after it
// maps or unmaps such memory (never from a signal handler).
using ViewFinder = int (*)(uint64_t address, uint64_t* out, int max, bool may_block);
void set_view_finder(ViewFinder finder);
void refresh_watchpoint_views();
void set_watchpoint_log_writes(int writes);
// True when [address, address + size) touches a page a watchpoint protects:
// emulated functions that let the kernel write guest memory (read() into a
// title buffer) go through a bounce buffer there.
bool overlaps_watched_page(uint64_t address, uint64_t size);
void unregister_dump_thread();

}  // namespace xbem::platform
