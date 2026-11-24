/* The run window shows the title's back buffer. A device, a back buffer made
   the way the title makes it (CreateCommittedResource3, device slot 127, heap
   flag ALLOW_DISPLAY), a test pattern written through Map. With "wait" on the
   command line the program stays four seconds, so the window can be seen:
     xbem run present.exe -- wait */
#include "guest.h"

IMP HRESULT D3D12XboxCreateDevice(void* adapter, const void* params, const void* iid, void** device);
IMP char* GetCommandLineA(void);

typedef void** Vt;
#define VT(o) (*(Vt*)(o))
typedef HRESULT (*CreateCommitted3Fn)(void*, const void*, uint32_t, const void*, uint32_t, const void*, void*,
                                      uint32_t, const void*, const void*, void**);
typedef HRESULT (*MapFn)(void*, uint32_t, const void*, void**);

static int contains(const char* s, const char* word) {
  for (; *s; ++s) {
    int i = 0;
    while (word[i] && s[i] == word[i]) ++i;
    if (!word[i]) return 1;
  }
  return 0;
}

DWORD guest_main(void* peb) {
  (void)peb;
  static const uint8_t iid[16] = {0x7d, 0x12};
  static const uint8_t resource_iid[16] = {0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7, 0x59, 0x40,
                                           0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad};
  void* device = 0;
  CHECK(D3D12XboxCreateDevice(0, 0, iid, &device) == 0 && device);
  uint8_t tex[56] = {0};
  tex[0] = 3; /* TEXTURE2D */
  *(uint64_t*)(tex + 16) = 3840;
  *(uint32_t*)(tex + 24) = 2160;
  *(uint16_t*)(tex + 28) = 1;
  *(uint16_t*)(tex + 30) = 1;
  *(uint32_t*)(tex + 32) = 28; /* R8G8B8A8_UNORM */
  *(uint32_t*)(tex + 36) = 1;
  static const uint32_t heap_props[5] = {1, 0, 0, 1, 1};
  void* back_buffer = 0;
  CHECK(((CreateCommitted3Fn)VT(device)[127])(device, heap_props, 0x8 /* ALLOW_DISPLAY */, tex, 2, 0, 0, 0, 0,
                                              resource_iid, &back_buffer) == 0 && back_buffer);
  uint8_t* pixels = 0;
  CHECK(((MapFn)VT(back_buffer)[8])(back_buffer, 0, 0, (void**)&pixels) == 0 && pixels);
  /* Red grows to the right, green downwards; a white frame and a blue square. */
  for (uint32_t y = 0; y < 2160; ++y)
    for (uint32_t x = 0; x < 3840; ++x) {
      uint8_t* p = pixels + (size_t)y * 15360 + (size_t)x * 4;
      const int frame = x < 32 || y < 32 || x >= 3808 || y >= 2128;
      const int square = x >= 1600 && x < 2240 && y >= 760 && y < 1400;
      p[0] = frame ? 255 : square ? 0 : (uint8_t)(x * 255 / 3839);
      p[1] = frame ? 255 : square ? 0 : (uint8_t)(y * 255 / 2159);
      p[2] = frame || square ? 255 : 64;
      p[3] = 255;
    }
  if (contains(GetCommandLineA(), "wait")) Sleep(4000);
  return (DWORD)finish("present");
}
