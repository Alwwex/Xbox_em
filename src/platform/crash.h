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

}  // namespace xbem::platform
