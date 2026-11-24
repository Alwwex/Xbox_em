#include "platform/window.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ------------------------------------------------- pixel conversion (any host)

namespace xbem::platform {
namespace {

uint8_t to8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

float half_to_float(uint16_t h) {
  const int exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
  float v;
  if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
  else if (exponent == 31) v = mantissa ? 0.0f : 65504.0f;  // NaN shows black, infinity white
  else v = std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
  return (h & 0x8000) ? -v : v;
}

}  // namespace

bool convert_surface_row(const uint8_t* row, uint32_t format, uint32_t width, uint32_t step, uint32_t* out) {
  switch (format) {
    case 27: case 28: case 29: case 30: case 31:  // R8G8B8A8 typeless/unorm/srgb/uint/snorm
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* p = row + static_cast<size_t>(x) * step * 4;
        out[x] = static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
      }
      return true;
    case 87: case 88: case 90: case 91: case 92: case 93:  // B8G8R8A8 / B8G8R8X8
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* p = row + static_cast<size_t>(x) * step * 4;
        out[x] = static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[0];
      }
      return true;
    case 23: case 24: case 25:  // R10G10B10A2
      for (uint32_t x = 0; x < width; ++x) {
        uint32_t v;
        std::memcpy(&v, row + static_cast<size_t>(x) * step * 4, 4);
        out[x] = ((v >> 2) & 0xff) << 16 | ((v >> 12) & 0xff) << 8 | ((v >> 22) & 0xff);
      }
      return true;
    case 9: case 10:  // R16G16B16A16 typeless/float (HDR back buffers)
      for (uint32_t x = 0; x < width; ++x) {
        uint16_t c[3];
        std::memcpy(c, row + static_cast<size_t>(x) * step * 8, 6);
        out[x] = static_cast<uint32_t>(to8(half_to_float(c[0]))) << 16 |
                 static_cast<uint32_t>(to8(half_to_float(c[1]))) << 8 | to8(half_to_float(c[2]));
      }
      return true;
    default:
      return false;
  }
}

}  // namespace xbem::platform

#if defined(_WIN32)

namespace xbem::platform {
bool window_open(const std::string&, WindowStatusProvider, std::string& reason) {
  reason = "the window is only implemented for Linux / WSL2 hosts";
  return false;
}
void window_set_surface(const void*, uint32_t, uint32_t, uint64_t, uint32_t) {}
bool window_is_open() { return false; }
}  // namespace xbem::platform

#else

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <thread>
#include <type_traits>

#include "common/log.h"

namespace xbem::platform {
namespace {

// The few Xlib entry points the window needs, bound with dlsym. Xlib's
// handles are opaque here: Display*, GC, Visual* and XImage* are void*,
// XIDs (Window, Atom) unsigned long.
using XID = unsigned long;
struct Xlib {
  int (*InitThreads)();
  void* (*OpenDisplay)(const char*);
  int (*DefaultScreen)(void*);
  XID (*RootWindow)(void*, int);
  unsigned long (*BlackPixel)(void*, int);
  XID (*CreateSimpleWindow)(void*, XID, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
  int (*StoreName)(void*, XID, const char*);
  int (*SelectInput)(void*, XID, long);
  int (*MapWindow)(void*, XID);
  void* (*DefaultGC)(void*, int);
  int (*SetForeground)(void*, void*, unsigned long);
  int (*FillRectangle)(void*, XID, void*, int, int, unsigned, unsigned);
  int (*DrawString)(void*, XID, void*, int, int, const char*, int);
  void* (*DefaultVisual)(void*, int);
  int (*DefaultDepth)(void*, int);
  void* (*CreateImage)(void*, void*, unsigned, int, int, char*, unsigned, unsigned, int, int);
  int (*PutImage)(void*, XID, void*, void*, int, int, int, int, unsigned, unsigned);
  int (*Pending)(void*);
  int (*NextEvent)(void*, void*);
  int (*Flush)(void*);
  XID (*InternAtom)(void*, const char*, int);
  int (*SetWMProtocols)(void*, XID, XID*, int);
  int (*DestroyWindow)(void*, XID);
  int (*CloseDisplay)(void*);
};

bool bind_xlib(Xlib& x, std::string& reason) {
  void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    reason = "libX11.so.6 is not installed (sudo apt install libx11-6)";
    return false;
  }
  bool ok = true;
  const auto bind = [&](auto& fn, const char* name) {
    void* p = dlsym(lib, name);
    if (!p) ok = false;
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
  };
  bind(x.InitThreads, "XInitThreads");
  bind(x.OpenDisplay, "XOpenDisplay");
  bind(x.DefaultScreen, "XDefaultScreen");
  bind(x.RootWindow, "XRootWindow");
  bind(x.BlackPixel, "XBlackPixel");
  bind(x.CreateSimpleWindow, "XCreateSimpleWindow");
  bind(x.StoreName, "XStoreName");
  bind(x.SelectInput, "XSelectInput");
  bind(x.MapWindow, "XMapWindow");
  bind(x.DefaultGC, "XDefaultGC");
  bind(x.SetForeground, "XSetForeground");
  bind(x.FillRectangle, "XFillRectangle");
  bind(x.DrawString, "XDrawString");
  bind(x.DefaultVisual, "XDefaultVisual");
  bind(x.DefaultDepth, "XDefaultDepth");
  bind(x.CreateImage, "XCreateImage");
  bind(x.PutImage, "XPutImage");
  bind(x.Pending, "XPending");
  bind(x.NextEvent, "XNextEvent");
  bind(x.Flush, "XFlush");
  bind(x.InternAtom, "XInternAtom");
  bind(x.SetWMProtocols, "XSetWMProtocols");
  bind(x.DestroyWindow, "XDestroyWindow");
  bind(x.CloseDisplay, "XCloseDisplay");
  if (!ok) reason = "libX11.so.6 lacks functions the window needs";
  return ok;
}

constexpr int kImageWidth = 960, kImageHeight = 540;  // a 3840x2160 surface at 1/4
constexpr int kLineHeight = 16, kStatusLines = 5;
constexpr int kStatusHeight = kLineHeight * kStatusLines + 8;
constexpr long kExposureMask = 1L << 15, kStructureNotifyMask = 1L << 17;
constexpr int kClientMessage = 33, kZPixmap = 2;

struct Surface {
  const void* pixels = nullptr;
  uint32_t width = 0, height = 0, format = 0;
  uint64_t pitch = 0;
};

std::mutex g_surface_mutex;
Surface g_surface;
std::atomic<bool> g_open{false};

struct WindowState {
  Xlib x{};
  void* display = nullptr;
  XID window = 0;
  void* gc = nullptr;
  void* image = nullptr;
  int screen = 0;
  WindowStatusProvider status = nullptr;
  std::vector<uint32_t> pixels = std::vector<uint32_t>(static_cast<size_t>(kImageWidth) * kImageHeight);
};

void text(WindowState& s, int x, int y, unsigned long color, const std::string& line) {
  s.x.SetForeground(s.display, s.gc, color);
  s.x.DrawString(s.display, s.window, s.gc, x, y, line.c_str(), static_cast<int>(line.size()));
}

void draw(WindowState& s) {
  s.x.SetForeground(s.display, s.gc, 0x202428);
  s.x.FillRectangle(s.display, s.window, s.gc, 0, 0, kImageWidth, kStatusHeight);
  std::vector<std::string> lines = s.status ? s.status() : std::vector<std::string>{};
  Surface surface;
  {
    std::lock_guard lock(g_surface_mutex);
    surface = g_surface;
  }
  if (!surface.pixels) {
    lines.push_back("no display surface yet (the title has not created its back buffers)");
  } else {
    lines.push_back(std::format("display surface {}x{}, format {} - black until a GPU renderer exists",
                                surface.width, surface.height, surface.format));
  }
  for (size_t i = 0; i < lines.size() && i < kStatusLines; ++i)
    text(s, 8, kLineHeight * static_cast<int>(i + 1), i == 0 ? 0xffffff : 0xb8c0c8, lines[i]);

  if (!surface.pixels) {
    s.x.SetForeground(s.display, s.gc, 0);
    s.x.FillRectangle(s.display, s.window, s.gc, 0, kStatusHeight, kImageWidth, kImageHeight);
    return;
  }
  const uint32_t step = std::max<uint32_t>(
      1, std::max((surface.width + kImageWidth - 1) / kImageWidth, (surface.height + kImageHeight - 1) / kImageHeight));
  const uint32_t w = std::min<uint32_t>(surface.width / step, kImageWidth);
  const uint32_t h = std::min<uint32_t>(surface.height / step, kImageHeight);
  bool known = true;
  for (uint32_t y = 0; y < h && known; ++y)
    known = convert_surface_row(static_cast<const uint8_t*>(surface.pixels) + y * step * surface.pitch, surface.format,
                                w, step, s.pixels.data() + static_cast<size_t>(y) * kImageWidth);
  if (!known) {
    s.x.SetForeground(s.display, s.gc, 0);
    s.x.FillRectangle(s.display, s.window, s.gc, 0, kStatusHeight, kImageWidth, kImageHeight);
    text(s, 8, kStatusHeight + 20, 0xff8080, std::format("surface format {} is not shown yet", surface.format));
    return;
  }
  s.x.PutImage(s.display, s.window, s.gc, s.image, 0, 0, 0, kStatusHeight, w, h);
}

void window_thread(WindowState* s) {
  XID wm_delete = s->x.InternAtom(s->display, "WM_DELETE_WINDOW", 0);
  s->x.SetWMProtocols(s->display, s->window, &wm_delete, 1);
  while (g_open.load()) {
    while (s->x.Pending(s->display)) {
      long event[24] = {};
      s->x.NextEvent(s->display, event);
      // XClientMessageEvent: type at +0, data.l[0] at +56.
      if (static_cast<int>(event[0]) == kClientMessage && static_cast<XID>(event[7]) == wm_delete) {
        g_open.store(false);
        XBEM_INFO("window", "window closed; the title keeps running (Ctrl+C ends it)");
      }
    }
    if (!g_open.load()) break;
    draw(*s);
    s->x.Flush(s->display);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  s->x.DestroyWindow(s->display, s->window);
  s->x.CloseDisplay(s->display);
}

}  // namespace

bool window_open(const std::string& title, WindowStatusProvider status, std::string& reason) {
  if (g_open.load()) return true;
  const char* display_name = std::getenv("DISPLAY");
  if (!display_name || !*display_name) {
    reason = "DISPLAY is not set (no X server: WSLg on Windows 11, or an X server, provides one)";
    return false;
  }
  auto* s = new WindowState;  // lives as long as the window thread
  if (!bind_xlib(s->x, reason)) {
    delete s;
    return false;
  }
  s->x.InitThreads();
  s->display = s->x.OpenDisplay(nullptr);
  if (!s->display) {
    reason = std::format("cannot connect to the X server on DISPLAY={}", display_name);
    delete s;
    return false;
  }
  s->screen = s->x.DefaultScreen(s->display);
  if (s->x.DefaultDepth(s->display, s->screen) < 24) {
    reason = "the X screen has fewer than 24 bits per pixel";
    s->x.CloseDisplay(s->display);
    delete s;
    return false;
  }
  s->status = status;
  s->window = s->x.CreateSimpleWindow(s->display, s->x.RootWindow(s->display, s->screen), 0, 0, kImageWidth,
                                      kStatusHeight + kImageHeight, 0, 0, s->x.BlackPixel(s->display, s->screen));
  s->x.StoreName(s->display, s->window, title.c_str());
  s->x.SelectInput(s->display, s->window, kExposureMask | kStructureNotifyMask);
  s->gc = s->x.DefaultGC(s->display, s->screen);
  s->image = s->x.CreateImage(s->display, s->x.DefaultVisual(s->display, s->screen),
                              static_cast<unsigned>(s->x.DefaultDepth(s->display, s->screen)), kZPixmap, 0,
                              reinterpret_cast<char*>(s->pixels.data()), kImageWidth, kImageHeight, 32, 0);
  if (!s->image) {
    reason = "XCreateImage failed";
    s->x.CloseDisplay(s->display);
    delete s;
    return false;
  }
  s->x.MapWindow(s->display, s->window);
  s->x.Flush(s->display);
  g_open.store(true);
  std::thread(window_thread, s).detach();
  return true;
}

void window_set_surface(const void* pixels, uint32_t width, uint32_t height, uint64_t pitch, uint32_t format) {
  std::lock_guard lock(g_surface_mutex);
  g_surface = {pixels, width, height, format, pitch};
}

bool window_is_open() { return g_open.load(); }

}  // namespace xbem::platform

#endif
