#pragma once

// "Probe" COM objects for undocumented console interfaces.
//
// A probe object accepts a call to any vtable slot, logs it (slot number,
// arguments, a hex dump of the memory pointer arguments point to, call site)
// and reports success. When a call looks like it returns a new interface -
// a pointer to an interface ID inside a loaded image followed by an output
// pointer - a new probe object is written to the output. Titles keep running
// through APIs we cannot implement yet, and the log documents the interface
// they use: the vtable order, argument shapes and structures.

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <string>

namespace xbem::hle {

constexpr int kProbeSlots = 512;  // D3D12.X devices have far more methods than the PC interface

constexpr uint64_t kProbeMagic = 0x45424F5250424558ull;  // "XBEPROBE"

// Console runtime objects are wider than their vtables: titles read and write
// the runtime's fields directly (a command list's write cursor at +0x18 and
// its limits at +0x28/+0x30, flag bytes at +0x140, pipeline state copied to
// +0x544 - measured by the Leonida project on the shipped DLLs). Every probe
// object starts with this zeroed area so those accesses never reach the
// emulator's own fields.
constexpr size_t kProbeConsoleArea = 0x800;

struct ProbeHeader {
  void* const* vtable;
  uint64_t magic = kProbeMagic;
  uint8_t console[kProbeConsoleArea - 16] = {};
};
static_assert(sizeof(ProbeHeader) == kProbeConsoleArea);

struct ProbeObject : ProbeHeader {
  std::string name;
  std::atomic<uint32_t> refs{1};
  std::array<std::atomic<int>, kProbeSlots> calls{};
  void* state = nullptr;  // per-class data of typed objects
};

using ProbeVtable = std::array<void*, kProbeSlots>;

// The all-probe vtable; typed classes copy it and override known slots.
const ProbeVtable& default_probe_vtable();

// A copy of the all-probe vtable with some slots replaced by typed methods.
inline ProbeVtable make_probe_vtable(std::initializer_list<std::pair<int, void*>> overrides) {
  ProbeVtable v = default_probe_vtable();
  for (const auto& [slot, f] : overrides) v[static_cast<size_t>(slot)] = f;
  return v;
}

// Creates a probe object; `name` labels its calls in the log ("dstorage.factory").
void* make_probe_object(const std::string& name, const ProbeVtable* vtable = nullptr, void* state = nullptr);

// True when `address` points into a read-only data section of a loaded image,
// where interface IDs (riid arguments) live.
bool is_interface_id_pointer(uint64_t address);

// True when `address` is inside the calling guest thread's stack.
bool in_current_guest_stack(uint64_t address);

// Prints the calling thread's recent probe-object calls (crash reports, dumps).
void print_probe_history(void (*write)(const char*));

// What an untyped probe slot does with a call (logging, and objects handed
// out by shape unless `hand_out_objects` is false - methods known to return
// no interface must not have one written into the caller's variables), for
// typed methods that only observe a call.
uint64_t probe_dispatch(ProbeObject* self, int slot, const uint64_t* args, uint64_t return_address,
                        bool hand_out_objects = true);

// Logs a call of a typed method the way probe slots do (first few calls per slot).
void probe_log(ProbeObject* self, int slot, const std::string& text, uint64_t return_address);

}  // namespace xbem::hle
