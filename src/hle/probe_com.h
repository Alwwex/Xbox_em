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

#include <cstdint>
#include <string>

namespace xbem::hle {

// Creates a probe object; `name` labels its calls in the log ("dstorage.factory").
void* make_probe_object(const std::string& name);

}  // namespace xbem::hle
