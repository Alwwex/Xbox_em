#pragma once

namespace xbem::hle {

// The headless D3D12.X device (see d3d12_device.cpp): GPU work completes
// instantly, nothing is drawn, shader binaries can be captured.
// `iid`: the interface ID the title asked for (QueryInterface with it returns the device).
void* create_headless_d3d12_device(const void* iid);

}  // namespace xbem::hle
