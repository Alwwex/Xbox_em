#pragma once

// The host GPU through OpenGL 4.6 (EGL + X11), loaded at run time like the
// run window's libX11: building and running xbem headless needs no GL
// libraries. On WSL2 the Mesa "d3d12" Gallium driver runs OpenGL on the
// Windows GPU driver (e.g. an NVIDIA GeForce), so xbem selects it when
// /dev/dxg exists and GALLIUM_DRIVER is not set.
//
// One GPU thread owns the context; other threads hand it work (gl_run).
// The run window gives it a child window to present into.

#include <cstdint>
#include <functional>
#include <string>

namespace xbem::gpu {

// Starts the GPU thread with a context that presents into X11 window `xid`
// (of size width x height). On success `info` names the renderer; on
// failure it says why (the run window then draws with XPutImage).
bool gl_start(unsigned long xid, int width, int height, std::string& info);

// True once gl_start succeeded.
bool gl_active();

// Shows `pixels` (0x00RRGGBB, width x height, rows packed) scaled to the
// presentation window; returns at once (the GPU thread uploads a copy).
void gl_present_pixels(const uint32_t* pixels, int width, int height);

// Saves the next presented frame, as read back from the GPU, to a PPM file
// (development: what the window shows, for runs nobody watches).
void gl_request_screenshot(const std::string& path);

// Runs `job` on the GPU thread, where the context is current.
void gl_run(std::function<void()> job);

}  // namespace xbem::gpu
