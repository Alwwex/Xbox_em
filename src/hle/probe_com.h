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
#include <atomic>
#include <cstdint>
#include <string>

namespace xbem::hle {

constexpr int kProbeSlots = 128;

struct ProbeObject {
  void* const* vtable;
  std::string name;
  std::atomic<uint32_t> refs{1};
  std::array<std::atomic<int>, kProbeSlots> calls{};
  void* state = nullptr;  // per-class data of typed objects
};

using ProbeVtable = std::array<void*, kProbeSlots>;

// The all-probe vtable; typed classes copy it and override known slots.
const ProbeVtable& default_probe_vtable();

// Creates a probe object; `name` labels its calls in the log ("dstorage.factory").
void* make_probe_object(const std::string& name, const ProbeVtable* vtable = nullptr, void* state = nullptr);

// Logs a call of a typed method the way probe slots do (first few calls per slot).
void probe_log(ProbeObject* self, int slot, const std::string& text, uint64_t return_address);

}  // namespace xbem::hle
