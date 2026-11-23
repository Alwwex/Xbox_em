// Window manager (user32 / ext-ms-win-rtcore-ntuser-* API sets).
//
// Console titles have one implicit full-screen "core window"; some engines
// still create a classic window and pump messages. Windows here are just
// handles: creation succeeds, the message queue is always empty, and the
// default window procedure does nothing.

#include <atomic>

#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;

std::atomic<uint64_t> g_next_window{0x10010};
std::atomic<uint16_t> g_next_atom{0xC001};

uint16_t XBEM_GUEST_ABI hle_RegisterClassExA(const void*) { return g_next_atom.fetch_add(1); }
uint16_t XBEM_GUEST_ABI hle_RegisterClassExW(const void*) { return g_next_atom.fetch_add(1); }
uint16_t XBEM_GUEST_ABI hle_RegisterClassA(const void*) { return g_next_atom.fetch_add(1); }
uint16_t XBEM_GUEST_ABI hle_RegisterClassW(const void*) { return g_next_atom.fetch_add(1); }
BOOL XBEM_GUEST_ABI hle_UnregisterClassA(const void*, uint64_t) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_UnregisterClassW(const void*, uint64_t) { return TRUE; }

uint64_t create_window(const std::string& title, int32_t w, int32_t h) {
  const uint64_t hwnd = g_next_window.fetch_add(0x10);
  XBEM_INFO("user32", "CreateWindowEx(\"{}\", {}x{}) -> {:#x} (window is not displayed)", title, w, h, hwnd);
  return hwnd;
}

uint64_t XBEM_GUEST_ABI hle_CreateWindowExA(DWORD, const char*, const char* title, DWORD, int32_t, int32_t,
                                            int32_t w, int32_t h, uint64_t, uint64_t, uint64_t, void*) {
  return create_window(title && reinterpret_cast<uint64_t>(title) >= 0x10000 ? narrow(title) : "", w, h);
}
uint64_t XBEM_GUEST_ABI hle_CreateWindowExW(DWORD, const char16_t*, const char16_t* title, DWORD, int32_t, int32_t,
                                            int32_t w, int32_t h, uint64_t, uint64_t, uint64_t, void*) {
  return create_window(title && reinterpret_cast<uint64_t>(title) >= 0x10000 ? narrow(title) : "", w, h);
}
BOOL XBEM_GUEST_ABI hle_DestroyWindow(uint64_t) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_ShowWindow(uint64_t, int32_t) { return FALSE; }  // "was previously hidden"
BOOL XBEM_GUEST_ABI hle_UpdateWindow(uint64_t) { return TRUE; }
int64_t XBEM_GUEST_ABI hle_DefWindowProcA(uint64_t, UINT, uint64_t, int64_t) { return 0; }
int64_t XBEM_GUEST_ABI hle_DefWindowProcW(uint64_t, UINT, uint64_t, int64_t) { return 0; }

// The message queue is always empty.
BOOL XBEM_GUEST_ABI hle_PeekMessageA(void*, uint64_t, UINT, UINT, UINT) { return FALSE; }
BOOL XBEM_GUEST_ABI hle_PeekMessageW(void*, uint64_t, UINT, UINT, UINT) { return FALSE; }
BOOL XBEM_GUEST_ABI hle_TranslateMessage(const void*) { return FALSE; }
int64_t XBEM_GUEST_ABI hle_DispatchMessageA(const void*) { return 0; }
int64_t XBEM_GUEST_ABI hle_DispatchMessageW(const void*) { return 0; }
void XBEM_GUEST_ABI hle_PostQuitMessage(int32_t code) { XBEM_INFO("user32", "PostQuitMessage({})", code); }
BOOL XBEM_GUEST_ABI hle_PostMessageW(uint64_t, UINT, uint64_t, int64_t) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_PostMessageA(uint64_t, UINT, uint64_t, int64_t) { return TRUE; }

int32_t XBEM_GUEST_ABI hle_GetSystemMetrics(int32_t index) {
  switch (index) {
    case 0: return 3840;  // SM_CXSCREEN
    case 1: return 2160;  // SM_CYSCREEN
    default: return 0;
  }
}

int32_t XBEM_GUEST_ABI hle_MessageBoxA(uint64_t, const char* text, const char* caption, UINT) {
  XBEM_WARN("user32", "MessageBox \"{}\": {}", caption ? narrow(caption) : "", text ? narrow(text) : "");
  return 1;  // IDOK
}
int32_t XBEM_GUEST_ABI hle_MessageBoxW(uint64_t, const char16_t* text, const char16_t* caption, UINT) {
  XBEM_WARN("user32", "MessageBox \"{}\": {}", caption ? narrow(caption) : "", text ? narrow(text) : "");
  return 1;
}

}  // namespace

ModuleDef make_user32_module() {
  ModuleDef m;
  m.name = "user32.dll";
  m.description = "Window manager: windows are invisible handles, the message queue is always empty";
  m.exports = {
      XBEM_HLE(RegisterClassExA),  XBEM_HLE(RegisterClassExW),  XBEM_HLE(RegisterClassA),   XBEM_HLE(RegisterClassW),
      XBEM_HLE(UnregisterClassA),  XBEM_HLE(UnregisterClassW),  XBEM_HLE(CreateWindowExA),  XBEM_HLE(CreateWindowExW),
      XBEM_HLE(DestroyWindow),     XBEM_HLE(ShowWindow),        XBEM_HLE(UpdateWindow),     XBEM_HLE(DefWindowProcA),
      XBEM_HLE(DefWindowProcW),    XBEM_HLE(PeekMessageA),      XBEM_HLE(PeekMessageW),     XBEM_HLE(TranslateMessage),
      XBEM_HLE(DispatchMessageA),  XBEM_HLE(DispatchMessageW),  XBEM_HLE(PostQuitMessage),  XBEM_HLE(PostMessageW),
      XBEM_HLE(PostMessageA),      XBEM_HLE(GetSystemMetrics),  XBEM_HLE(MessageBoxA),      XBEM_HLE(MessageBoxW),
  };
  return m;
}

}  // namespace xbem::hle
