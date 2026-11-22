#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace xbem::gpu {

// The GPU layer is where most of the remaining work of a Series X|S emulator
// lies (see docs/ARCHITECTURE.md, section "GPU"). Titles talk to the console
// GPU through the Xbox flavour of Direct3D 12 ("D3D12.X") and ship shaders
// precompiled to native RDNA2 machine code. A working backend needs:
//
//   1. an implementation of the D3D12.X COM interfaces (device, queues,
//      command lists, heaps, pipeline state) on top of Vulkan or PC D3D12,
//   2. a way to obtain usable shaders - either DXIL from the developer or an
//      RDNA2 ISA -> SPIR-V/DXIL recompiler,
//   3. presentation (swap chain -> host window).
//
// This interface is the seam where such a backend plugs in.
struct BackendInfo {
  std::string name;
  bool can_render = false;
  std::string notes;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual BackendInfo info() const = 0;
  virtual bool initialize(std::string& error) = 0;
};

// Known names: "null". Returns nullptr for unknown names.
std::unique_ptr<Backend> create_backend(std::string_view name);

// Backend selected for the running process (created on first use).
Backend& active_backend();

}  // namespace xbem::gpu
