#pragma once

namespace xbem::hle {

// The headless D3D12.X device (see d3d12_device.cpp): GPU work completes
// instantly, nothing is drawn, shader binaries can be captured.
void* create_headless_d3d12_device();

}  // namespace xbem::hle
