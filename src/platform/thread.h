#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace xbem::platform {

// Whether guest code can be executed on this host. Execution needs a way to
// point the GS segment at an emulated TEB (Linux x86-64, including WSL2).
bool guest_execution_supported(std::string* reason = nullptr);

using ThreadFn = void (*)(void*);

struct NativeThread {
  uintptr_t handle = 0;
};

// Starts a host thread with a stack of at least `stack_size` bytes.
bool start_thread(size_t stack_size, ThreadFn fn, void* arg, NativeThread& out);
void join_thread(NativeThread& thread);
void detach_thread(NativeThread& thread);

// Usable stack range of the calling thread: [low, high).
bool current_stack_bounds(uintptr_t& low, uintptr_t& high);

// Points the GS segment base of the calling thread at `teb`.
bool set_guest_teb(void* teb);

void set_current_thread_name(const std::string& name);
uint32_t current_processor_number();
uint32_t host_process_id();
unsigned hardware_threads();
void yield_thread();

}  // namespace xbem::platform
