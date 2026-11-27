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
  uintptr_t id = 0;  // kept after detach_thread (affinity changes)
};

// Starts a host thread with a stack of at least `stack_size` bytes.
bool start_thread(size_t stack_size, ThreadFn fn, void* arg, NativeThread& out);
void join_thread(NativeThread& thread);
void detach_thread(NativeThread& thread);
// Restricts a thread to the host processors in `mask` (bit n = processor n;
// bits past the host's processors are dropped). False when it cannot.
bool set_thread_affinity(const NativeThread& thread, uint64_t mask);
// Background work (shader builds): the calling thread runs at a low priority.
void lower_current_thread_priority();

// Usable stack range of the calling thread: [low, high).
bool current_stack_bounds(uintptr_t& low, uintptr_t& high);

// Points the GS segment base of the calling thread at `teb`.
bool set_guest_teb(void* teb);

void set_current_thread_name(const std::string& name);
uint32_t current_processor_number();
uint32_t host_process_id();
unsigned hardware_threads();
void yield_thread();

// Makes all writes of every thread of the process visible to the others
// (Windows FlushProcessWriteBuffers: a process-wide memory barrier).
void flush_process_write_buffers();

}  // namespace xbem::platform
