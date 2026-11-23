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

// Prints extra per-thread context (e.g. the thread's recent emulated calls)
// into crash reports and thread dumps, using the given writer.
using ThreadContextPrinter = void (*)(void (*write)(const char*));
void set_thread_context_printer(ThreadContextPrinter printer);
void unregister_dump_thread();

}  // namespace xbem::platform
