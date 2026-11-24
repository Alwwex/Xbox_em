#pragma once

// The run window: status lines and the title's display surface (its back
// buffer), scaled to fit. X11 through libX11.so.6, loaded at run time, so
// building xbem and running it headless (tests, CI, no display) need no X
// libraries; WSLg provides the X server on Windows 11.
//
// Nothing is rendered into the surface yet (there is no GPU backend), so it
// shows what the title's memory holds - black until rendering exists.

#include <cstdint>
#include <string>
#include <vector>

namespace xbem::platform {

// Lines shown at the top of the window, asked for on every refresh (from the
// window's own thread).
using WindowStatusProvider = std::vector<std::string> (*)();

// Opens the window on its own thread. Returns false, with the reason, when no
// display can be used (DISPLAY unset, libX11 missing, no X server).
bool window_open(const std::string& title, WindowStatusProvider status, std::string& reason);

// The surface the window shows: pixels in guest memory, rows of `pitch` bytes,
// a DXGI `format` (8-bit RGBA/BGRA, 10:10:10:2 and 16-bit float RGBA are
// converted; others are reported). Can be called again to switch surfaces.
void window_set_surface(const void* pixels, uint32_t width, uint32_t height, uint64_t pitch, uint32_t format);

// For tests and logs: true while the window is open.
bool window_is_open();

// Converts `width` pixels of one row of a DXGI surface to 0x00RRGGBB words;
// false for a format it does not know. Exposed for tests.
bool convert_surface_row(const uint8_t* row, uint32_t format, uint32_t width, uint32_t step, uint32_t* out);

}  // namespace xbem::platform
