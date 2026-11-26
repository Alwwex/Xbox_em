/* Console-title APIs added for real games: xmem (including aliased physical
 * pages), locale/NLS, time conversion, processor topology, PE resources, COM
 * marshaling, offline Winsock (imported by ordinal), WinHTTP URL parsing and
 * the window manager API sets. */
#include "guest.h"

typedef uint16_t WORD;

/* xmem.dll */
IMP void* XMemAlloc(size_t, uint64_t);
IMP void XMemFree(void*, uint64_t);
IMP void* XMemVirtualAlloc(void*, size_t, DWORD, uint64_t, DWORD);
IMP int32_t XMemAllocatePhysicalPages(uint64_t, uint64_t*, uint64_t*);
IMP void* XMemMapPhysicalPages(void*, uint64_t, const uint64_t*);
IMP int32_t XMemFreePhysicalPages(uint64_t, const uint64_t*);
IMP int32_t XMemMapPhysicalPagesScatter(void**, uint64_t, const uint64_t*);
IMP BOOL XMemIsToolsMemAvailable(void);

/* xg_xs.dll */
IMP HRESULT XGCreateTextureComputer(const void* desc, void** out);
IMP HRESULT XGSuggestSwizzleMode(const void* desc, uint64_t reserved, uint32_t* mode);

/* kernelx.dll */
IMP DWORD FormatMessageA(DWORD, const void*, DWORD, DWORD, char*, DWORD, void*);
IMP int GetLocaleInfoEx(const WCHAR*, DWORD, WCHAR*, int);
IMP int CompareStringEx(const WCHAR*, DWORD, const WCHAR*, int, const WCHAR*, int, void*, void*, uint64_t);
IMP int lstrcmpiW(const WCHAR*, const WCHAR*);
IMP int LCMapStringEx(const WCHAR*, DWORD, const WCHAR*, int, WCHAR*, int, void*, void*, uint64_t);
IMP int GetDateFormatEx(const WCHAR*, DWORD, const WORD*, const WCHAR*, WCHAR*, int, const WCHAR*);
IMP int GetTimeFormatEx(const WCHAR*, DWORD, const WORD*, const WCHAR*, WCHAR*, int);
IMP BOOL SystemTimeToFileTime(const WORD*, uint64_t*);
IMP BOOL FileTimeToSystemTime(const uint64_t*, WORD*);
IMP BOOL GetLogicalProcessorInformationEx(DWORD, uint8_t*, DWORD*);
IMP void* FindResourceW(HANDLE, const WCHAR*, const WCHAR*);
IMP void* LoadResource(HANDLE, void*);
IMP void* LockResource(void*);
IMP DWORD SizeofResource(HANDLE, void*);
IMP HRESULT CoMarshalInterThreadInterfaceInStream(const void*, void*, void**);
IMP HRESULT CoGetInterfaceAndReleaseStream(void*, const void*, void**);
IMP void GetCurrentThreadStackLimits(uint64_t*, uint64_t*);
IMP HANDLE OpenThread(DWORD, BOOL, DWORD);
IMP void FlushProcessWriteBuffers(void);
IMP void* LocalFree(void*);
IMP BOOL InitializeProcThreadAttributeList(void*, DWORD, DWORD, size_t*);
IMP size_t VirtualQuery(const void*, void*, size_t);
IMP void* VirtualAlloc2(HANDLE, void*, size_t, DWORD, DWORD, void*, DWORD);

/* ws2_32.dll (by ordinal) */
IMP uint16_t htons(uint16_t);
IMP uint32_t inet_addr(const char*);
IMP uint64_t socket(int, int, int);
IMP int WSAGetLastError(void);
IMP int WSAStartup(WORD, void*);
IMP int WSACleanup(void);
IMP int getaddrinfo(const char*, const char*, const void*, void**);

/* winhttp.dll */
IMP void* WinHttpOpen(const WCHAR*, DWORD, const WCHAR*, const WCHAR*, DWORD);
IMP void* WinHttpConnect(void*, const WCHAR*, WORD, DWORD);
IMP void* WinHttpOpenRequest(void*, const WCHAR*, const WCHAR*, const WCHAR*, const WCHAR*, const WCHAR**, DWORD);
IMP BOOL WinHttpSendRequest(void*, const WCHAR*, DWORD, void*, DWORD, DWORD, uint64_t);
IMP BOOL WinHttpCloseHandle(void*);
IMP BOOL WinHttpCrackUrl(const WCHAR*, DWORD, DWORD, void*);

/* oleaut32.dll (by ordinal) */
IMP WCHAR* SysAllocString(const WCHAR*);
IMP void SysFreeString(WCHAR*);
IMP uint32_t SysStringLen(const WCHAR*);

/* d3d12_xs.dll */
IMP HRESULT D3D12XboxCreateDevice(void* adapter, const void* params, const void* iid, void** device);
IMP HRESULT D3D12SerializeVersionedRootSignature(const void* desc, void** blob, void** error_blob);
IMP HRESULT D3D12CreateRootSignatureDeserializer(const void* data, size_t size, const void* iid, void** out);

/* xcrdapi.dll */
IMP HRESULT DStorageGetFactory(const void* iid, void** factory);

/* xgameruntime.dll */
IMP HRESULT XSystemGetXboxLiveSandboxId(size_t size, char* buffer, size_t* used);
IMP HRESULT QueryApiImpl(const void* clsid, const void* iid, void** object);

/* AcpHal.dll, xapu.dll (audio co-processor) */
IMP HRESULT ApuCreateHeap(uint32_t cached_bytes, uint32_t non_cached_bytes);
IMP HRESULT ApuAlloc(void** va, uint32_t* apu_address, uint32_t size, uint32_t alignment, uint32_t flags);
IMP HRESULT ApuFree(void* va);
IMP uint32_t ApuMapVirtualAddress(void* va);
IMP void* ApuMapApuAddress(uint32_t apu_address);
IMP BOOL ApuIsVirtualAddressValid(void* va, uint32_t alignment);
IMP HRESULT AcpHalAllocateShapeContexts(void* contexts);
IMP HRESULT AcpHalReleaseShapeContexts(void);
IMP HRESULT AcpHalCreate(void** hal);
IMP HRESULT XApuConnect(const void* params, uint64_t* shared, void** handle);
IMP HRESULT XApuEnqueueCommand(void* handle, const void* command, uint32_t flags);
IMP uint32_t XApuGetResultCount(void* handle, uint32_t kind);
IMP HRESULT XApuDisconnect(void* handle);

/* combase (through kernelx.dll) */
IMP HRESULT CoCreateInstance(const void* clsid, void* outer, DWORD context, const void* iid, void** out);
IMP void CoTaskMemFree(void*);

/* gameinput.dll (ordinal 1) */
IMP HRESULT GameInputCreate(void** game_input);

/* ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll */
IMP uint64_t CreateWindowExA(DWORD, const char*, const char*, DWORD, int, int, int, int, uint64_t, uint64_t,
                             uint64_t, void*);
IMP BOOL PeekMessageA(void*, uint64_t, unsigned, unsigned, unsigned);

#define W(s) ((const WCHAR*)L##s)

static int wstreq(const WCHAR* a, const WCHAR* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}

static int starts_with(const char* s, const char* prefix) {
  while (*prefix)
    if (*s++ != *prefix++) return 0;
  return 1;
}

/* Exported: run --watch at:console.exe!watch_here:rcx+OFFSET arms a watchpoint
   on memory the test maps (the address is only known at run time). */
__attribute__((noinline, dllexport)) void watch_here(volatile uint8_t* p) {
  (void)p;
  __asm__ volatile("" ::: "memory");
}

/* ------------------------------------------------------------------ xmem */

static void test_xmem(void) {
  uint8_t* p = XMemAlloc(1000, 0);
  CHECK(p != 0);
  CHECK(((uintptr_t)p & 63) == 0);
  memset(p, 0xAB, 1000);
  XMemFree(p, 0);
  uint8_t* big = XMemAlloc(1 << 20, 0);
  CHECK(big && ((uintptr_t)big & 0xFFFF) == 0);
  XMemFree(big, 0);
  CHECK(!XMemIsToolsMemAvailable());

  /* (address, size, type, console flags, protection) */
  uint8_t* va = XMemVirtualAlloc(0, 0x20000, MEM_RESERVE | 0x20000000 /*MEM_LARGE_PAGES*/, 0x3, PAGE_READWRITE);
  CHECK(va != 0);

  uint64_t count = 1, pages[1] = {0};
  CHECK(XMemAllocatePhysicalPages(0, &count, pages) >= 0);
  CHECK(count == 1 && pages[0] != 0);
  /* The same physical page mapped twice: both views see the same bytes. */
  CHECK(XMemMapPhysicalPages(va, 1, pages) == va);
  CHECK(XMemMapPhysicalPages(va + 0x10000, 1, pages) == va + 0x10000);
  /* volatile: the compiler may otherwise reorder accesses it believes are disjoint */
  volatile uint8_t* v = va;
  v[0x123] = 0x5A;
  CHECK(v[0x10000 + 0x123] == 0x5A);
  v[0x10000 + 0xFFFF] = 0x77;
  CHECK(v[0xFFFF] == 0x77);
  /* run --watch at:console.exe!watch_here:rcx+0x120 watches the other view too */
  watch_here(va);
  v[0x10000 + 0x120] = 0x66;
  CHECK(v[0x120] == 0x66);
  CHECK(XMemMapPhysicalPages(va, 1, 0) == va); /* unmap */
  CHECK(XMemFreePhysicalPages(1, pages) >= 0);
  CHECK(VirtualFree(va, 0, MEM_RELEASE));

  /* Guest memory lies below 2^40, as on the console (40-bit GPU addresses);
   * released memory is handed out again, zeroed. */
  {
    uint8_t* a = VirtualAlloc(0, 0x30000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(a && (uint64_t)(uintptr_t)a < (1ull << 40) && ((uintptr_t)a & 0xffff) == 0);
    uint8_t* big = XMemVirtualAlloc(0, 0x40000000, MEM_RESERVE | 0x20000000, 0x3, PAGE_READWRITE);
    CHECK(big && (uint64_t)(uintptr_t)big + 0x40000000 <= (1ull << 40));
    a[0x2ffff] = 0x5a;
    CHECK(VirtualFree(a, 0, MEM_RELEASE));
    uint8_t* again = VirtualAlloc(a, 0x30000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(again == a && again[0x2ffff] == 0);
    CHECK(VirtualFree(again, 0, MEM_RELEASE));
    CHECK(VirtualFree(big, 0, MEM_RELEASE));
  }

  /* The title's layout, (flags, &count, pages), frees exactly those pages; a
     call naming pages that are not allocated frees nothing - freeing pages in
     use would hand them to another allocation, which overwrites them. */
  {
    typedef int32_t (*free3_fn)(uint64_t, uint64_t*, const uint64_t*);
    free3_fn free3 = (free3_fn)(void*)XMemFreePhysicalPages;
    uint8_t* w = XMemVirtualAlloc(0, 0x30000, MEM_RESERVE | 0x20000000 /*MEM_LARGE_PAGES*/, 0x3, PAGE_READWRITE);
    CHECK(w != 0);
    uint64_t n2 = 2, keep[2] = {0}, n1 = 1, other[1] = {0}, again[1] = {0};
    CHECK(XMemAllocatePhysicalPages(0x380, &n2, keep) >= 0);
    CHECK(XMemAllocatePhysicalPages(0x380, &n1, other) >= 0);
    CHECK(XMemMapPhysicalPages(w, 2, keep) == w);
    volatile uint8_t* vw = w;
    vw[0x10] = 0x11;
    vw[0x10010] = 0x22;
    CHECK(free3(0x380, &n1, other) >= 0); /* frees `other` only */
    uint64_t junk[2] = {keep[0] + 0x1000000, 5}, two = 2;
    CHECK(free3(0x280, &two, junk) >= 0); /* nothing allocated named: nothing freed */
    CHECK(XMemAllocatePhysicalPages(0x380, &n1, again) >= 0);
    CHECK(again[0] != keep[0] && again[0] != keep[1]); /* never a page in use */
    CHECK(XMemMapPhysicalPages(w + 0x20000, 1, again) == w + 0x20000);
    vw[0x20010] = 0x33;
    CHECK(vw[0x10] == 0x11 && vw[0x10010] == 0x22 && vw[0x20010] == 0x33);
    CHECK(XMemMapPhysicalPages(w, 3, 0) == w);
    CHECK(free3(0x380, &n2, keep) >= 0 && free3(0x380, &n1, again) >= 0);
    CHECK(VirtualFree(w, 0, MEM_RELEASE));
  }

  /* Page frame numbers are in 4 KiB units (a 64 KiB page's is a multiple of
     16): rounding one down to its 64 KiB page, as GTA VI does for the page
     array of its scatter mappings, keeps distinct pages distinct. */
  {
    uint8_t* sp = XMemVirtualAlloc(0, 0x20000, MEM_RESERVE | 0x20000000 /*MEM_LARGE_PAGES*/, 0x3, PAGE_READWRITE);
    uint64_t n = 2, pg[2] = {0};
    CHECK(sp != 0 && XMemAllocatePhysicalPages(0x380, &n, pg) >= 0);
    CHECK((pg[0] & 0xF) == 0 && (pg[1] & 0xF) == 0 && pg[0] != pg[1]);
    void* vas[2] = {sp, sp + 0x10000};
    const uint64_t frames[2] = {pg[0] & ~0xFull, pg[1] & ~0xFull};
    CHECK(XMemMapPhysicalPagesScatter(vas, 2, frames) >= 0);
    volatile uint8_t* vs = sp;
    vs[0x40] = 0xA1;
    vs[0x10040] = 0xB2;
    CHECK(vs[0x40] == 0xA1 && vs[0x10040] == 0xB2); /* two pages, not one */
    /* 0 entries unmap their addresses (the title unmaps this way and asserts success) */
    const uint64_t zeros[2] = {0, 0};
    CHECK(XMemMapPhysicalPagesScatter(vas, 2, zeros) >= 0);
    CHECK(XMemMapPhysicalPagesScatter(vas, 2, frames) >= 0); /* the pages kept their bytes */
    CHECK(vs[0x40] == 0xA1 && vs[0x10040] == 0xB2);
    CHECK(XMemMapPhysicalPages(sp, 2, 0) == sp);
    CHECK(XMemFreePhysicalPages(2, pg) >= 0);
    CHECK(VirtualFree(sp, 0, MEM_RELEASE));
  }

  /* Placeholders: reserve, split, replace with an allocation, turn back into a
   * placeholder, coalesce, release (the pattern GTA-engine descriptor heaps use). */
  enum { RESERVE_PH = 0x40000, REPLACE_PH = 0x4000, PRESERVE_PH = 0x2, COALESCE_PH = 0x1 };
  uint8_t* ph = XMemVirtualAlloc(0, 0x300000, MEM_RESERVE | RESERVE_PH, 0x7, 0x1 /*PAGE_NOACCESS*/);
  CHECK(ph != 0);
  CHECK(!VirtualFree(ph, 0x100000, MEM_RELEASE)); /* partial release without PRESERVE fails */
  CHECK(VirtualFree(ph, 0x100000, MEM_RELEASE | PRESERVE_PH));
  CHECK(VirtualFree(ph + 0x100000, 0x100000, MEM_RELEASE | PRESERVE_PH));
  uint64_t mbi[6];
  CHECK(VirtualQuery(ph + 0x100000, mbi, sizeof mbi) == 48);
  CHECK((DWORD)mbi[4] == MEM_RESERVE);
  CHECK(VirtualAlloc(ph + 0x100000, 0x100000, MEM_RESERVE | MEM_COMMIT | REPLACE_PH, PAGE_READWRITE) == ph + 0x100000);
  ph[0x100000 + 0x42] = 9;
  CHECK(ph[0x100042] == 9);
  CHECK(!VirtualAlloc(ph + 0x100000, 0x100000, MEM_RESERVE | REPLACE_PH, 0x1)); /* no longer a placeholder */
  CHECK(VirtualFree(ph + 0x100000, 0, MEM_RELEASE | PRESERVE_PH));
  uint64_t req[3] = {0, 0, 0x400000}; /* MEM_ADDRESS_REQUIREMENTS: 4 MiB alignment */
  uint64_t ext[2] = {1, (uint64_t)(uintptr_t)req};
  uint8_t* aligned = VirtualAlloc2(0, 0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, ext, 1);
  CHECK(aligned && ((uintptr_t)aligned & 0x3FFFFF) == 0);
  CHECK(VirtualFree(aligned, 0, MEM_RELEASE));
  CHECK(VirtualFree(ph, 0x200000, MEM_RELEASE | COALESCE_PH)); /* merging a subset */
  CHECK(VirtualFree(ph, 0x300000, MEM_RELEASE | COALESCE_PH));
  CHECK(VirtualFree(ph, 0, MEM_RELEASE));
}

/* ------------------------------------------------- console CPU (Zen 2) */

/* Zen 2 instructions a host CPU may lack: cache hints are patched into NOPs,
 * CLZERO is emulated (an Intel host faults on it). */
static void test_zen2_instructions(void) {
  static uint8_t buf[256] __attribute__((aligned(64)));
  for (int i = 0; i < 256; ++i) buf[i] = 0xEE;
  uint8_t* p = buf + 64;
  __asm__ volatile(".byte 0x66, 0x0f, 0xae, 0x30" ::"a"(p) : "memory"); /* clwb [rax] */
  __asm__ volatile(".byte 0x66, 0x0f, 0xae, 0x38" ::"a"(p) : "memory"); /* clflushopt [rax] */
  __asm__ volatile(".byte 0x0f, 0x01, 0xfc" ::"a"(p + 5) : "memory");   /* clzero: line of rax */
  CHECK(buf[63] == 0xEE && buf[64] == 0 && buf[127] == 0 && buf[128] == 0xEE);

  /* SSE4a bit fields (AMD only, emulated on Intel): INSERTQ / EXTRQ */
  uint64_t value = 0x1122334455667788ull, field = 0xAB, out = 0, control = 0x0808;
  __asm__ volatile("movq %1, %%xmm1\n\tmovq %2, %%xmm2\n\t"
                   ".byte 0xf2, 0x0f, 0x78, 0xca, 0x08, 0x10\n\t" /* insertq xmm1, xmm2, 8, 16 */
                   "movq %%xmm1, %0"
                   : "=r"(out) : "r"(value), "r"(field) : "xmm1", "xmm2");
  CHECK(out == 0x1122334455AB7788ull);
  __asm__ volatile("movq %1, %%xmm1\n\t"
                   ".byte 0x66, 0x0f, 0x78, 0xc1, 0x08, 0x10\n\t" /* extrq xmm1, 8, 16 */
                   "movq %%xmm1, %0"
                   : "=r"(out) : "r"(value) : "xmm1");
  CHECK(out == 0x66);
  __asm__ volatile("movq %1, %%xmm9\n\tmovq %2, %%xmm2\n\t"
                   ".byte 0x66, 0x44, 0x0f, 0x79, 0xca\n\t" /* extrq xmm9, xmm2 (len 8, index 8) */
                   "movq %%xmm9, %0"
                   : "=r"(out) : "r"(value), "r"(control) : "xmm9", "xmm2");
  CHECK(out == 0x77);
}

typedef void** Vt;
#define VT(o) (*(Vt*)(o))

/* ------------------------------------------------------------------- XG */

/* Texture uploads: the computer copies linear rows into the emulator's
 * linear D3D12 layout (rows aligned to 256 bytes, subresources to 512). */
static uint8_t mip1_data_for_xg[100];

static void test_xg(void) {
  for (int i = 0; i < 100; ++i) mip1_data_for_xg[i] = (uint8_t)(i + 1);
  static uint8_t xg_desc[64];
  uint32_t mode = 0xFFFF;
  CHECK(XGSuggestSwizzleMode(xg_desc, 0, &mode) == 0 && mode == 0);
  void* computer = 0;
  CHECK(XGCreateTextureComputer(xg_desc, &computer) == 0 && computer);
  typedef HRESULT (*CopyFn)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
  static uint8_t gpu[0x20000];
  static uint8_t mip0[100 * 4], mip1[50 * 2];
  for (int i = 0; i < (int)sizeof mip0; ++i) mip0[i] = (uint8_t)i;
  for (int i = 0; i < (int)sizeof mip1; ++i) mip1[i] = (uint8_t)(0x80 + i);
  /* R8 100x4 (pitch 100) then 50x2 (pitch 50) */
  CHECK(((CopyFn)VT(computer)[8])(computer, gpu, 0, 0, mip0, 100, 400) == 0);
  CHECK(((CopyFn)VT(computer)[8])(computer, gpu, 0, 1, mip1, 50, 100) == 0);
  CHECK(gpu[0] == 0 && gpu[99] == 99 && gpu[256] == 100 && gpu[3 * 256 + 99] == (uint8_t)399);
  CHECK(gpu[1024] == 0x80 && gpu[1024 + 256 + 49] == 0x80 + 99); /* subresource 1 at 4 x 256 = 1024 */
  static uint8_t back[100];
  CHECK(((CopyFn)VT(computer)[10])(computer, back, 0, 1, gpu, 50, 100) == 0);
  CHECK(memcmp(back, mip1, 100) == 0);
  typedef uint32_t (*RefFn)(void*);
  CHECK(((RefFn)VT(computer)[1])(computer) == 0); /* Release */

  /* A described texture: R8 100x4 with 2 mips. XG_RESOURCE_LAYOUT: header
   * (size, alignment, mips, planes), plane 0 at 0x18 with its mips from
   * +0x38, 0x70 bytes each (size, offset, 2D size, pitch in bytes at +0x1c). */
  static uint8_t desc[64];
  memset(desc, 0, sizeof desc);
  *(uint32_t*)desc = 3;             /* TEXTURE2D */
  *(uint64_t*)(desc + 16) = 100;    /* width */
  *(uint32_t*)(desc + 24) = 4;      /* height */
  *(uint16_t*)(desc + 28) = 1;      /* array size */
  *(uint16_t*)(desc + 30) = 2;      /* mips */
  *(uint32_t*)(desc + 32) = 61;     /* R8_UNORM */
  *(uint32_t*)(desc + 36) = 1;      /* samples */
  CHECK(XGCreateTextureComputer(desc, &computer) == 0 && computer);
  typedef HRESULT (*LayoutFn)(void*, uint8_t*);
  static uint8_t layout[0x1b38];
  memset(layout, 0xcc, sizeof layout);
  CHECK(((LayoutFn)VT(computer)[2])(computer, layout) == 0);
  const uint8_t* plane = layout + 0x18;
  const uint8_t* mip1_layout = plane + 0x38 + 0x70;
  CHECK(*(uint64_t*)layout == 1536 && *(uint32_t*)(layout + 0x10) == 2 && *(uint32_t*)(layout + 0x14) == 1);
  CHECK(*(uint64_t*)(plane + 0x38) == 1024 && *(uint64_t*)(plane + 0x40) == 0 && *(uint32_t*)(plane + 0x38 + 0x1c) == 256);
  CHECK(*(uint64_t*)mip1_layout == 512 && *(uint64_t*)(mip1_layout + 8) == 1024 && *(uint32_t*)(mip1_layout + 0x1c) == 256);
  CHECK(*(uint32_t*)(mip1_layout + 0x30) == 50 && *(uint32_t*)(mip1_layout + 0x34) == 2);
  memset(gpu, 0, sizeof gpu);
  CHECK(((CopyFn)VT(computer)[8])(computer, gpu, 0, 1, mip1_data_for_xg, 50, 100) == 0);
  CHECK(gpu[1024] == 1 && gpu[1024 + 256 + 49] == 100);
  CHECK(((RefFn)VT(computer)[1])(computer) == 0);

  /* Slot 10 with pointers: GetResourceTiling (D3D12's standard 64 KiB tiles),
   * as GTA VI sizes its streamed textures. BC7 1024x1024 with 9 mips: tiles of
   * 64x64 blocks (256x256 texels); mips 1024, 512, 256 take 16 + 4 + 1 tiles,
   * the 6 smaller ones are packed into one more: 22 tiles. */
  *(uint64_t*)(desc + 16) = 1024;
  *(uint32_t*)(desc + 24) = 1024;
  *(uint16_t*)(desc + 30) = 9;
  *(uint32_t*)(desc + 32) = 98; /* BC7_UNORM */
  CHECK(XGCreateTextureComputer(desc, &computer) == 0 && computer);
  typedef HRESULT (*TilingFn)(void*, uint32_t*, uint8_t*, uint32_t*, uint32_t*, uint32_t, uint8_t*);
  uint32_t tiles = 0, shape[3] = {0}, count = 16;
  uint8_t packed[12] = {0}, tilings[16 * 12];
  memset(tilings, 0xcc, sizeof tilings);
  CHECK(((TilingFn)VT(computer)[10])(computer, &tiles, packed, shape, &count, 0, tilings) == 0);
  CHECK(tiles == 22 && packed[0] == 3 && packed[1] == 6 && *(uint32_t*)(packed + 4) == 1 && *(uint32_t*)(packed + 8) == 21);
  CHECK(shape[0] == 256 && shape[1] == 256 && shape[2] == 1 && count == 9);
  CHECK(*(uint32_t*)tilings == 4 && *(uint16_t*)(tilings + 4) == 4 && *(uint32_t*)(tilings + 8) == 0);
  CHECK(*(uint32_t*)(tilings + 12) == 2 && *(uint32_t*)(tilings + 12 + 8) == 16);
  CHECK(*(uint32_t*)(tilings + 24) == 1 && *(uint32_t*)(tilings + 24 + 8) == 20);
  CHECK(*(uint32_t*)(tilings + 36) == 0 && *(uint32_t*)(tilings + 36 + 8) == 0xFFFFFFFFu);
  CHECK(((RefFn)VT(computer)[1])(computer) == 0);
}

/* --------------------------------------------------------------- locale */

static void test_locale(void) {
  WCHAR buf[64];
  CHECK(GetLocaleInfoEx(W("en-US"), 0x5C /*LOCALE_SNAME*/, buf, 64) == 6);
  CHECK(wstreq(buf, W("en-US")));
  CHECK(GetLocaleInfoEx(0, 0x0E /*LOCALE_SDECIMAL*/, 0, 0) == 2);
  CHECK(CompareStringEx(0, 1 /*NORM_IGNORECASE*/, W("Hello"), -1, W("HELLO"), -1, 0, 0, 0) == 2);
  CHECK(CompareStringEx(0, 0, W("apple"), -1, W("Banana"), -1, 0, 0, 0) == 1);
  CHECK(CompareStringEx(0, 0, W("b"), 1, W("abc"), 3, 0, 0, 0) == 3);
  CHECK(lstrcmpiW(W("abc"), W("ABC")) == 0);
  CHECK(LCMapStringEx(0, 0x200 /*LCMAP_UPPERCASE*/, W("MiXed"), -1, buf, 64, 0, 0, 0) == 6);
  CHECK(wstreq(buf, W("MIXED")));

  const WORD st[8] = {2024, 3, 6, 9, 14, 5, 7, 250}; /* Saturday 2024-03-09 14:05:07.250 */
  CHECK(GetDateFormatEx(0, 0, st, W("yyyy-MM-dd dddd MMM"), buf, 64, 0) > 0);
  CHECK(wstreq(buf, W("2024-03-09 Saturday Mar")));
  CHECK(GetTimeFormatEx(0, 0, st, W("hh:mm:ss tt"), buf, 64) > 0);
  CHECK(wstreq(buf, W("02:05:07 PM")));
  CHECK(GetTimeFormatEx(0, 8 /*TIME_FORCE24HOURFORMAT*/, st, 0, buf, 64) > 0);
  CHECK(wstreq(buf, W("14:05:07")));

  char text[256];
  CHECK(FormatMessageA(0x1000 | 0x200, 0, 2, 0, text, sizeof text, 0) > 0);
  CHECK(starts_with(text, "The system cannot find the file specified."));
  uint64_t args[3] = {(uint64_t)(uintptr_t) "one", 2, 255};
  CHECK(FormatMessageA(0x400 | 0x2000, "%1 + %2!d! = %3!x!%0", 0, 0, text, sizeof text, args) == 12);
  CHECK(gstreq(text, "one + 2 = ff"));
  char* allocated = 0;
  CHECK(FormatMessageA(0x400 | 0x100, "abc%n", 0, 0, (char*)&allocated, 0, 0) == 5);
  CHECK(allocated && gstreq(allocated, "abc\r\n"));
  LocalFree(allocated);
}

/* ----------------------------------------------------------------- time */

static void test_time(void) {
  const WORD epoch[8] = {1970, 1, 0, 1, 0, 0, 0, 0};
  uint64_t ft = 0;
  CHECK(SystemTimeToFileTime(epoch, &ft));
  CHECK(ft == 116444736000000000ull);
  const WORD st[8] = {2024, 2, 0, 29, 23, 59, 58, 999};
  CHECK(SystemTimeToFileTime(st, &ft));
  WORD back[8];
  CHECK(FileTimeToSystemTime(&ft, back));
  CHECK(back[0] == 2024 && back[1] == 2 && back[3] == 29 && back[4] == 23 && back[5] == 59 && back[6] == 58 &&
        back[7] == 999);
  CHECK(back[2] == 4); /* Thursday */
}

/* ------------------------------------------------------------- topology */

static void test_topology(void) {
  DWORD len = 0;
  CHECK(!GetLogicalProcessorInformationEx(0xFFFF, 0, &len));
  CHECK(GetLastError() == 122 && len > 0);
  static uint8_t buf[16384];
  len = sizeof buf;
  CHECK(GetLogicalProcessorInformationEx(0 /*RelationProcessorCore*/, buf, &len));
  DWORD cores = 0;
  for (DWORD off = 0; off < len;) {
    DWORD rel, size;
    memcpy(&rel, buf + off, 4);
    memcpy(&size, buf + off + 4, 4);
    CHECK(rel == 0 && size == 48);
    ++cores;
    off += size;
  }
  CHECK(cores >= 1);
}

/* ------------------------------------------------------------ resources */

static void test_resources(void) {
  void* r = FindResourceW(0, (const WCHAR*)(uintptr_t)1, (const WCHAR*)(uintptr_t)10 /*RT_RCDATA*/);
  CHECK(r != 0);
  const char* data = LockResource(LoadResource(0, r));
  CHECK(data && gstreq(data, "xbem-resource-data"));
  CHECK(SizeofResource(0, r) == 19);
  void* named = FindResourceW(0, W("config"), (const WCHAR*)(uintptr_t)10);
  CHECK(named && gstreq(LockResource(LoadResource(0, named)), "named-resource"));
  CHECK(FindResourceW(0, (const WCHAR*)(uintptr_t)77, (const WCHAR*)(uintptr_t)10) == 0);
}

/* ----------------------------------------------------------------- COM */

typedef struct Obj Obj;
typedef struct {
  HRESULT (*QueryInterface)(Obj*, const void*, void**);
  uint32_t (*AddRef)(Obj*);
  uint32_t (*Release)(Obj*);
} ObjVtbl;
struct Obj {
  const ObjVtbl* vtbl;
  int refs;
};
static HRESULT obj_qi(Obj* o, const void* iid, void** out) {
  (void)iid;
  ++o->refs;
  *out = o;
  return 0;
}
static uint32_t obj_add_ref(Obj* o) { return (uint32_t)++o->refs; }
static uint32_t obj_release(Obj* o) { return (uint32_t)--o->refs; }
static const ObjVtbl kObjVtbl = {obj_qi, obj_add_ref, obj_release};

static void test_com(void) {
  Obj obj = {&kObjVtbl, 1};
  static const uint8_t iid[16] = {1};
  void* stream = 0;
  CHECK(CoMarshalInterThreadInterfaceInStream(iid, &obj, &stream) == 0 && stream);
  CHECK(obj.refs == 2);
  void* out = 0;
  CHECK(CoGetInterfaceAndReleaseStream(stream, iid, &out) == 0);
  CHECK(out == &obj && obj.refs == 2); /* +1 from QueryInterface, -1 from the released stream */
}

/* -------------------------------------------------------------- threads */

static void test_threads(void) {
  uint64_t lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  const uint64_t here = (uint64_t)(uintptr_t)&lo;
  CHECK(lo < here && here < hi);
  HANDLE t = OpenThread(0x1FFFFF, 0, GetCurrentThreadId());
  CHECK(t != 0);
  CHECK(CloseHandle(t));
  FlushProcessWriteBuffers();
  size_t size = 0;
  CHECK(!InitializeProcThreadAttributeList(0, 2, 0, &size) && size > 0);
}

/* -------------------------------------------------------------- network */

static void test_network(void) {
  uint8_t wsa[408];
  CHECK(WSAStartup(0x0202, wsa) == 0);
  CHECK(wsa[0] == 2 && wsa[1] == 2);
  CHECK(htons(0x1234) == 0x3412);
  CHECK(inet_addr("127.0.0.1") == 0x0100007Fu);
  CHECK(socket(2, 1, 6) == ~0ull);
  CHECK(WSAGetLastError() == 10050); /* WSAENETDOWN: offline */
  void* res = (void*)1;
  CHECK(getaddrinfo("example.com", "443", 0, &res) == 11001 && res == 0);
  CHECK(WSACleanup() == 0);

  struct {
    DWORD size, pad0;
    WCHAR* scheme;
    DWORD scheme_len;
    int scheme_id;
    WCHAR* host;
    DWORD host_len;
    WORD port, pad1;
    WCHAR* user;
    DWORD user_len, pad2;
    WCHAR* password;
    DWORD password_len, pad3;
    WCHAR* path;
    DWORD path_len, pad4;
    WCHAR* extra;
    DWORD extra_len, pad5;
  } uc;
  memset(&uc, 0, sizeof uc);
  uc.size = sizeof uc;
  uc.host_len = uc.path_len = uc.extra_len = uc.scheme_len = (DWORD)-1;
  CHECK(sizeof uc == 104);
  CHECK(WinHttpCrackUrl(W("https://user:pw@example.com:8443/a/b?x=1"), 0, 0, &uc));
  CHECK(uc.scheme_id == 2 && uc.port == 8443);
  CHECK(uc.host_len == 11 && uc.host[0] == 'e' && uc.host[10] == 'm');
  CHECK(uc.path_len == 4 && uc.path[0] == '/' && uc.path[3] == 'b');
  CHECK(uc.extra_len == 4 && uc.extra[0] == '?');

  void* session = WinHttpOpen(W("xbem-test"), 0, 0, 0, 0);
  CHECK(session != 0);
  void* conn = WinHttpConnect(session, W("example.com"), 443, 0);
  void* req = WinHttpOpenRequest(conn, W("GET"), W("/"), 0, 0, 0, 0);
  CHECK(conn && req);
  CHECK(!WinHttpSendRequest(req, 0, 0, 0, 0, 0, 0));
  CHECK(GetLastError() == 12029); /* ERROR_WINHTTP_CANNOT_CONNECT */
  CHECK(WinHttpCloseHandle(req) && WinHttpCloseHandle(conn) && WinHttpCloseHandle(session));
}

/* ----------------------------------------------------- oleaut32, ntuser */

static void test_misc(void) {
  WCHAR* b = SysAllocString(W("abc"));
  CHECK(b && SysStringLen(b) == 3);
  SysFreeString(b);
  CHECK(CreateWindowExA(0, "cls", "xbem test window", 0, 0, 0, 1280, 720, 0, 0, 0, 0) != 0);
  uint8_t msg[48];
  CHECK(!PeekMessageA(msg, 0, 0, 0, 1));
}

/* --------------------------------------------------------- GameInput */

typedef HRESULT (*GiCurrentReading)(void*, uint32_t, void*, void**);
typedef uint64_t (*GiTimestamp)(void*);
typedef HRESULT (*GiRegisterDevice)(void*, void*, uint32_t, uint32_t, uint32_t, void*, void*, uint64_t*);
typedef HRESULT (*GiCreateDispatcher)(void*, void**);
typedef _Bool (*GiDispatch)(void*, uint64_t);
typedef uint32_t (*Release)(void*);

/* The audio start-up of the title: APU heap, XMA shape contexts, the HAL
 * object (not COM: slot 0 connects) and an XApu queue. */
static void test_audio(void) {
  void* va = 0;
  uint32_t apu = 0;
  CHECK(ApuAlloc(&va, &apu, 0x100, 0x800, 0) < 0); /* no heap yet */
  CHECK(ApuCreateHeap(0x100000, 0x20000) == 0);
  CHECK(ApuCreateHeap(0x100000, 0x20000) < 0); /* only once */
  CHECK(ApuAlloc(&va, &apu, 0x1e000, 0x800, 2) == 0 && va && apu && apu % 0x800 == 0);
  CHECK(((uint8_t*)va)[0] == 0 && ((uint8_t*)va)[0x1dfff] == 0);
  ((uint8_t*)va)[0x1dfff] = 0x5a;
  CHECK(ApuMapVirtualAddress(va) == apu && ApuMapApuAddress(apu) == va);
  CHECK(ApuMapVirtualAddress((uint8_t*)va + 0x40) == apu + 0x40);
  CHECK(ApuIsVirtualAddressValid(va, 0x80) && !ApuIsVirtualAddressValid((uint8_t*)va + 4, 0x40));
  CHECK(!ApuIsVirtualAddressValid(&apu, 0) && ApuMapVirtualAddress(&apu) == 0xffffffffu);
  /* the non-cached part is full: falls back to the cached one */
  void* second = 0;
  uint32_t second_apu = 0;
  CHECK(ApuAlloc(&second, &second_apu, 0x10000, 0x800, 2) == 0 && second && second != va);
  void* big = 0;
  CHECK(ApuAlloc(&big, 0, 0x200000, 0x800, 0) < 0 && !big); /* larger than the heap */
  CHECK(ApuFree(va) == 0 && ApuFree(va) < 0 && !ApuIsVirtualAddressValid(va, 0));
  void* again = 0;
  CHECK(ApuAlloc(&again, &apu, 0x1e000, 0x800, 2) == 0 && again == va && ((uint8_t*)again)[0x1dfff] == 0);
  CHECK(ApuFree(again) == 0 && ApuFree(second) == 0);

  /* SHAPE_CONTEXTS: six counts, then six array pointers */
  struct {
    uint32_t count[6];
    void* array[6];
  } shape = {{0, 0, 0, 0, 350, 0}, {0}};
  CHECK(AcpHalAllocateShapeContexts(&shape) == 0 && shape.array[4] && !shape.array[0] && !shape.array[5]);
  CHECK(((uint8_t*)shape.array[4])[349 * 64 + 63] == 0);
  ((uint8_t*)shape.array[4])[349 * 64 + 63] = 1;
  CHECK(AcpHalReleaseShapeContexts() == 0);

  typedef HRESULT (*HalConnectFn)(void*, uint32_t, uint32_t);
  typedef HRESULT (*HalSubmitFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t*, uint32_t);
  void* hal = 0;
  CHECK(AcpHalCreate(&hal) == 0 && hal);
  CHECK(((HalConnectFn)VT(hal)[0])(hal, 0x1000, 0x1000) == 0);
  uint32_t arg = 0xc0;
  CHECK(((HalSubmitFn)VT(hal)[2])(hal, 2, 0xf0f0f0f0, 0xffffffff, &arg, 0) == 0);

  const uint32_t params[6] = {1, 0x100, 2, 0x104000, 0, 1};
  uint64_t shared[2] = {0, 0};
  void* queue = 0;
  CHECK(XApuConnect(params, shared, &queue) == 0 && queue && shared[0] && shared[1] == 0x104000);
  ((uint8_t*)(uintptr_t)shared[0])[0x103fff] = 1;
  const uint32_t command[16] = {1};
  CHECK(XApuEnqueueCommand(queue, command, 0) == 0 && XApuGetResultCount(queue, 0) == 0);
  CHECK(XApuDisconnect(queue) == 0);
}

/* A retail console's sandbox (the title asserts on unknown ones). */
static void test_sandbox(void) {
  char id[32];
  size_t used = 0;
  CHECK(XSystemGetXboxLiveSandboxId(sizeof id, id, &used) == 0 && used == 7);
  CHECK(id[0] == 'R' && id[1] == 'E' && id[2] == 'T' && id[3] == 'A' && id[4] == 'I' && id[5] == 'L' && id[6] == 0);
  CHECK(XSystemGetXboxLiveSandboxId(4, id, &used) < 0);

  /* The same through the runtime binding: XSystem's slot 4 */
  static const uint8_t xsystem_clsid[16] = {0x1a, 0xbd, 0x49, 0xe3, 0x20, 0xfc, 0x40, 0x4e,
                                            0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f};
  static const uint8_t xsystem_iid[16] = {0x2e, 0xcf, 0x61, 0x18, 0x8b, 0xe1, 0x34, 0x48,
                                          0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf};
  static const uint8_t unknown_clsid[16] = {1, 2, 3};
  typedef HRESULT (*IdFn)(void*, size_t, char*, size_t*);
  typedef uint32_t (*ReleaseFn)(void*);
  void* xsystem = 0;
  CHECK(QueryApiImpl(unknown_clsid, unknown_clsid, &xsystem) < 0 && !xsystem);
  CHECK(QueryApiImpl(xsystem_clsid, xsystem_iid, &xsystem) == 0 && xsystem);
  used = 0;
  CHECK(((IdFn)VT(xsystem)[4])(xsystem, sizeof id, id, &used) == 0 && used == 7 && id[0] == 'R' && id[5] == 'L');
  CHECK(((IdFn)VT(xsystem)[3])(xsystem, sizeof id, id, &used) < 0); /* console IDs need 39 bytes */
  char console_id[39];
  CHECK(((IdFn)VT(xsystem)[3])(xsystem, sizeof console_id, console_id, &used) == 0 && used == 39);
  typedef HRESULT (*NoArgFn)(void*);
  CHECK(((NoArgFn)VT(xsystem)[20])(xsystem) == (HRESULT)0x80004001); /* unknown slots: E_NOTIMPL */
  ((ReleaseFn)VT(xsystem)[2])(xsystem);

  /* XNetworking slot 15 GetConnectivityHint: initialized, no connectivity */
  static const uint8_t xnet_clsid[16] = {0x07, 0x69, 0xe5, 0x37, 0x10, 0x2f, 0xe8, 0x41,
                                         0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a};
  static const uint8_t xnet_iid[16] = {0xb2, 0x46, 0x23, 0xbf, 0xaf, 0x39, 0x58, 0x46,
                                       0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3};
  typedef HRESULT (*HintFn)(void*, uint8_t*);
  void* xnet = 0;
  uint8_t hint[16];
  for (int i = 0; i < 16; ++i) hint[i] = 0xcc;
  CHECK(QueryApiImpl(xnet_clsid, xnet_iid, &xnet) == 0 && xnet);
  CHECK(((HintFn)VT(xnet)[15])(xnet, hint) == 0);
  CHECK(*(uint32_t*)hint == 1 && hint[12] == 1 && hint[13] == 0 && hint[15] == 0);

  /* XPackage: packaged, fully installed, no other packages */
  static const uint8_t xpkg_clsid[16] = {0x16, 0x60, 0x40, 0xaf, 0x50, 0xe8, 0xa8, 0x4a,
                                         0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e};
  static const uint8_t xpkg_iid[16] = {0x6e, 0x55, 0x9d, 0x0b, 0x70, 0xab, 0xf3, 0x43,
                                       0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40};
  typedef _Bool (*IsPackagedFn)(void*);
  typedef HRESULT (*PkgIdFn)(void*, size_t, char*);
  typedef HRESULT (*MonitorFn)(void*, const char*, uint32_t, const void*, uint32_t, void*, void**);
  typedef void (*ProgressFn)(void*, void*, uint64_t*);
  typedef HRESULT (*EnumFn)(void*, uint32_t, uint32_t, void*, void*);
  void* xpkg = 0;
  char pkg_id[33];
  CHECK(QueryApiImpl(xpkg_clsid, xpkg_iid, &xpkg) == 0 && xpkg);
  CHECK(((IsPackagedFn)VT(xpkg)[4])(xpkg));
  CHECK(((PkgIdFn)VT(xpkg)[3])(xpkg, sizeof pkg_id, pkg_id) == 0 && pkg_id[0]);
  void* monitor = 0;
  CHECK(((MonitorFn)VT(xpkg)[5])(xpkg, pkg_id, 0, 0, 1000, 0, &monitor) == 0 && monitor);
  uint64_t progress[4] = {0};
  ((ProgressFn)VT(xpkg)[7])(xpkg, monitor, progress);
  CHECK(progress[0] && progress[1] == progress[0] && ((uint8_t*)progress)[24] == 1 && ((uint8_t*)progress)[25] == 1);
  CHECK(((EnumFn)VT(xpkg)[39])(xpkg, 1, 0, 0, (void*)test_sandbox) == 0);
}

/* XAsync, XTaskQueue, XThread ({073b7dcb-...}) and XUser ({01acd177-...})
 * through the runtime binding, in the order the title's runtime library
 * uses their slots. */
typedef struct XAsyncBlock_ {
  void* queue;
  void* context;
  void (*callback)(struct XAsyncBlock_*);
  uint8_t internal[32];
} XAsyncBlock_;
typedef struct {
  XAsyncBlock_* async;
  size_t buffer_size;
  void* buffer;
  void* context;
} XAsyncProviderData_;

static void* g_xasync;
static volatile int g_provider_ops[5];
static volatile int g_completed;
static volatile int g_queue_callbacks;

typedef HRESULT (*AsyncScheduleFn)(void*, XAsyncBlock_*, uint32_t);
typedef void (*AsyncCompleteFn)(void*, XAsyncBlock_*, HRESULT, size_t);

static HRESULT test_provider(uint32_t op, const XAsyncProviderData_* data) {
  if (op < 5) g_provider_ops[op]++;
  switch (op) {
    case 0: /* Begin */
      return ((AsyncScheduleFn)VT(g_xasync)[9])(g_xasync, data->async, 0);
    case 1: /* DoWork */
      ((AsyncCompleteFn)VT(g_xasync)[10])(g_xasync, data->async, 0, 4);
      return 0;
    case 2: /* GetResult */
      if (data->buffer_size < 4) return (HRESULT)0x80070057;
      *(uint32_t*)data->buffer = 0x1234;
      return 0;
    default:
      return 0;
  }
}

static void test_completion(XAsyncBlock_* block) { g_completed = 1 + (int)(intptr_t)block->context; }
static HRESULT test_failing_work(XAsyncBlock_* block) {
  (void)block;
  return (HRESULT)0x80004005;
}
static void test_queue_callback(void* context, BOOL canceled) {
  if (!canceled) g_queue_callbacks += (int)(intptr_t)context;
}

static void wait_for(volatile int* value, int expected) {
  for (int i = 0; i < 200 && *value != expected; ++i) Sleep(5);
}

static void test_gdk_async_user(void) {
  static const uint8_t xasync_clsid[16] = {0xcb, 0x7d, 0x3b, 0x07, 0xcf, 0x1f, 0x30, 0x40,
                                           0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28};
  static const uint8_t xuser_clsid[16] = {0x77, 0xd1, 0xac, 0x01, 0xf9, 0x91, 0x63, 0x47,
                                          0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0};
  static const uint8_t xuser2_iid[16] = {0xc0, 0xfa, 0xf4, 0xce, 0x76, 0x76, 0x94, 0x4a,
                                         0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74};
  typedef HRESULT (*CreateFn)(void*, uint32_t, uint32_t, void**);
  typedef HRESULT (*BeginFn)(void*, XAsyncBlock_*, void*, const void*, const char*, void*);
  typedef HRESULT (*StatusFn)(void*, XAsyncBlock_*, BOOL);
  typedef HRESULT (*SizeFn)(void*, XAsyncBlock_*, size_t*);
  typedef HRESULT (*ResultFn)(void*, XAsyncBlock_*, const void*, size_t, void*, size_t*);
  typedef HRESULT (*RunFn)(void*, XAsyncBlock_*, void*);
  typedef HRESULT (*SubmitFn)(void*, void*, uint32_t, void*, void*);
  typedef BOOL (*DispatchFn)(void*, void*, uint32_t, uint32_t);
  typedef void (*CloseFn)(void*, void*);
  typedef BOOL (*BoolFn)(void*);
  typedef void (*VoidFn)(void*);

  CHECK(QueryApiImpl(xasync_clsid, xasync_clsid, &g_xasync) == 0 && g_xasync);
  void* xa = g_xasync;
  ((VoidFn)VT(xa)[29])(xa); /* XThreadAssertNotTimeSensitive */
  CHECK(!((BoolFn)VT(xa)[27])(xa));

  /* A provider-driven operation on a thread-pool queue */
  void* queue = 0;
  CHECK(((CreateFn)VT(xa)[12])(xa, 1, 1, &queue) == 0 && queue);
  XAsyncBlock_ block = {queue, (void*)41, test_completion, {0}};
  CHECK(((BeginFn)VT(xa)[7])(xa, &block, 0, (const void*)test_provider, "test", (void*)test_provider) == 0);
  CHECK(((StatusFn)VT(xa)[3])(xa, &block, 1) == 0);
  size_t size = 0, used = 0;
  CHECK(((SizeFn)VT(xa)[4])(xa, &block, &size) == 0 && size == 4);
  uint32_t value = 0;
  CHECK(((ResultFn)VT(xa)[11])(xa, &block, (const void*)test_provider, 2, &value, &used) == (HRESULT)0x8007007A);
  CHECK(((ResultFn)VT(xa)[11])(xa, &block, (const void*)test_provider, 4, &value, &used) == 0);
  CHECK(value == 0x1234 && used == 4);
  wait_for(&g_completed, 42);
  CHECK(g_completed == 42);
  CHECK(g_provider_ops[0] == 1 && g_provider_ops[1] == 1 && g_provider_ops[2] == 1 && g_provider_ops[4] == 1);

  /* XAsyncRun: the work's result is the operation's status */
  XAsyncBlock_ run = {queue, 0, 0, {0}};
  CHECK(((RunFn)VT(xa)[6])(xa, &run, (void*)test_failing_work) == 0);
  CHECK(((StatusFn)VT(xa)[3])(xa, &run, 1) == (HRESULT)0x80004005);

  /* A manual queue runs callbacks only when dispatched */
  void* manual = 0;
  CHECK(((CreateFn)VT(xa)[12])(xa, 0, 0, &manual) == 0 && manual);
  CHECK(((SubmitFn)VT(xa)[18])(xa, manual, 0, (void*)5, (void*)test_queue_callback) == 0);
  CHECK(g_queue_callbacks == 0);
  CHECK(((DispatchFn)VT(xa)[16])(xa, manual, 0, 0) && g_queue_callbacks == 5);
  CHECK(!((DispatchFn)VT(xa)[16])(xa, manual, 0, 0));
  ((CloseFn)VT(xa)[17])(xa, manual);

  /* XUserAddAsync returns the signed-in default user */
  typedef HRESULT (*AddFn)(void*, uint32_t, XAsyncBlock_*);
  typedef HRESULT (*HandleOutFn)(void*, XAsyncBlock_*, void**);
  typedef HRESULT (*U64OutFn)(void*, void*, uint64_t*);
  typedef HRESULT (*U32OutFn)(void*, void*, uint32_t*);
  typedef HRESULT (*FindFn)(void*, uint64_t, void**);
  typedef HRESULT (*TagFn)(void*, void*, uint32_t, size_t, char*, size_t*);
  typedef HRESULT (*TokenFn)(void*, void*, uint32_t, const char*, const char*, size_t, const void*, size_t,
                             const void*, XAsyncBlock_*);
  void* xu = 0;
  CHECK(QueryApiImpl(xuser_clsid, xuser_clsid, &xu) == 0 && xu);
  g_completed = 0;
  XAsyncBlock_ add = {0, (void*)6, test_completion, {0}};
  CHECK(((AddFn)VT(xu)[7])(xu, 1, &add) == 0);
  CHECK(((StatusFn)VT(xa)[3])(xa, &add, 1) == 0);
  void* user = 0;
  CHECK(((HandleOutFn)VT(xu)[8])(xu, &add, &user) == 0 && user);
  wait_for(&g_completed, 7);
  CHECK(g_completed == 7);
  uint64_t xuid = 0, local_id = 0;
  uint32_t state = 9;
  CHECK(((U64OutFn)VT(xu)[11])(xu, user, &xuid) == 0 && xuid != 0);
  CHECK(((U64OutFn)VT(xu)[9])(xu, user, &local_id) == 0);
  void* found = 0;
  CHECK(((FindFn)VT(xu)[10])(xu, local_id, &found) == 0 && found == user);
  CHECK(((FindFn)VT(xu)[12])(xu, xuid + 1, &found) < 0);
  CHECK(((U32OutFn)VT(xu)[14])(xu, user, &state) == 0 && state == 0); /* SignedIn */
  void* xu2 = 0;
  char tag[16] = {0};
  CHECK(QueryApiImpl(xuser_clsid, xuser2_iid, &xu2) == 0 && xu2);
  CHECK(((TagFn)VT(xu2)[3])(xu2, user, 0, sizeof tag, tag, &used) == 0 && used == 5 && tag[0] == 'x' && tag[4] == 0);
  CHECK(((TagFn)VT(xu2)[3])(xu2, user, 0, 2, tag, &used) < 0);
  /* Xbox Live tokens fail: the console is offline */
  XAsyncBlock_ token = {0, 0, 0, {0}};
  CHECK(((TokenFn)VT(xu)[23])(xu, user, 0, "GET", "https://example.invalid/", 0, 0, 0, 0, &token) == 0);
  CHECK(((StatusFn)VT(xa)[3])(xa, &token, 1) < 0);
  ((CloseFn)VT(xu)[4])(xu, found);
  ((CloseFn)VT(xu)[4])(xu, user);
}

/* Core Audio as the title's audio start-up uses it: one render device, its
 * mix format, a Spatial Audio stream whose event the emulator signals every
 * 10 ms, and static objects with 480-frame buffers. */
static void test_audio_endpoint(void) {
  static const uint8_t clsid_enumerator[16] = {0x95, 0x03, 0xde, 0xbc, 0x2f, 0xe5, 0x7c, 0x46,
                                               0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e};
  static const uint8_t iid_enumerator[16] = {0xd2, 0x64, 0x56, 0xa9, 0x14, 0x96, 0x35, 0x4f,
                                             0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6};
  static const uint8_t iid_audio_client[16] = {0x4c, 0xad, 0xb9, 0x1c, 0xfa, 0xdb, 0x32, 0x4c,
                                               0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2};
  static const uint8_t iid_spatial_client[16] = {0x66, 0xe0, 0xf8, 0xbb, 0xaa, 0xaa, 0xbe, 0x49,
                                                 0x9a, 0x4d, 0xfd, 0x2a, 0x85, 0x8e, 0xa2, 0x7f};
  static const uint8_t iid_stream[16] = {0x73, 0xf4, 0xb5, 0xba, 0x23, 0xb4, 0x7b, 0x47,
                                         0x85, 0xf5, 0xb5, 0xa3, 0x32, 0xa0, 0x41, 0x53};
  typedef HRESULT (*EnumFn)(void*, uint32_t, uint32_t, void**);
  typedef HRESULT (*U32OutFn)(void*, uint32_t*);
  typedef HRESULT (*ItemFn)(void*, uint32_t, void**);
  typedef HRESULT (*ActivateFn)(void*, const void*, uint32_t, const void*, void**);
  typedef HRESULT (*PtrOutFn)(void*, void**);
  typedef HRESULT (*FormatFn)(void*, const void*);
  typedef HRESULT (*StreamFn)(void*, const void*, const void*, void**);
  typedef HRESULT (*NoArgFn)(void*);
  typedef HRESULT (*BeginFn)(void*, uint32_t*, uint32_t*);
  typedef HRESULT (*BufferFn)(void*, uint8_t**, uint32_t*);
  typedef HRESULT (*PositionFn)(void*, uint32_t, float*, float*, float*);

  void* enumerator = 0;
  CHECK(CoCreateInstance(clsid_enumerator, 0, 0x17, iid_enumerator, &enumerator) == 0 && enumerator);
  void* collection = 0;
  uint32_t count = 0;
  CHECK(((EnumFn)VT(enumerator)[3])(enumerator, 0, 1, &collection) == 0 && collection);
  CHECK(((U32OutFn)VT(collection)[3])(collection, &count) == 0 && count == 1);
  void* device = 0;
  CHECK(((ItemFn)VT(collection)[4])(collection, 0, &device) == 0 && device);
  void* capture = (void*)1;
  CHECK(((EnumFn)VT(enumerator)[4])(enumerator, 1, 0, &capture) < 0 && !capture); /* no capture devices */
  void* render = 0;
  CHECK(((EnumFn)VT(enumerator)[4])(enumerator, 0, 0, &render) == 0 && render == device);

  /* IAudioClient::GetMixFormat: WAVEFORMATEXTENSIBLE, 48 kHz float stereo */
  void* client = 0;
  CHECK(((ActivateFn)VT(device)[3])(device, iid_audio_client, 1, 0, &client) == 0 && client);
  uint8_t* mix = 0;
  CHECK(((PtrOutFn)VT(client)[8])(client, (void**)&mix) == 0 && mix);
  CHECK(*(uint16_t*)mix == 0xfffe && *(uint16_t*)(mix + 2) == 2 && *(uint32_t*)(mix + 4) == 48000);
  CoTaskMemFree(mix);

  /* ISpatialAudioClient */
  void* spatial = 0;
  CHECK(((ActivateFn)VT(device)[3])(device, iid_spatial_client, 1, 0, &spatial) == 0 && spatial);
  float x = 0, y = 1, z = 0;
  CHECK(((PositionFn)VT(spatial)[3])(spatial, 0x2, &x, &y, &z) == 0 && x < 0 && y == 0 && z < 0);
  void* formats = 0;
  CHECK(((PtrOutFn)VT(spatial)[6])(spatial, &formats) == 0 && formats);
  CHECK(((U32OutFn)VT(formats)[3])(formats, &count) == 0 && count == 1);
  const uint8_t* format = 0;
  CHECK(((ItemFn)VT(formats)[4])(formats, 0, (void**)&format) == 0 && format);
  CHECK(*(uint16_t*)format == 3 && *(uint16_t*)(format + 2) == 1 && *(uint32_t*)(format + 4) == 48000);
  CHECK(((FormatFn)VT(spatial)[8])(spatial, format) == 0);

  HANDLE event = CreateEventW(0, 0, 0, 0);
  struct {
    const void* format;
    uint32_t static_mask, min_dynamic, max_dynamic, category;
    HANDLE event;
    void* notify;
  } params = {format, 0x6, 0, 0, 6, event, 0};
  struct {
    uint16_t vt, r1, r2, r3;
    uint32_t size, pad;
    void* data;
  } pv = {0x41, 0, 0, 0, 0x28, 0, &params};
  void* stream = 0;
  CHECK(((StreamFn)VT(spatial)[10])(spatial, &pv, iid_stream, &stream) == 0 && stream);
  CHECK(WaitForSingleObject(event, 100) == WAIT_TIMEOUT); /* not started */
  CHECK(((NoArgFn)VT(stream)[5])(stream) == 0);           /* Start */
  CHECK(WaitForSingleObject(event, 1000) == WAIT_OBJECT_0);
  CHECK(WaitForSingleObject(event, 1000) == WAIT_OBJECT_0); /* and again 10 ms later */
  void* left = 0;
  void* none = (void*)1;
  CHECK(((ItemFn)VT(stream)[10])(stream, 0x2, &left) == 0 && left);
  CHECK(((ItemFn)VT(stream)[10])(stream, 0x8, &none) < 0 && !none); /* not in the static mask */
  uint32_t dynamic = 9, frames = 0, bytes = 0;
  uint8_t* buffer = 0;
  CHECK(((BeginFn)VT(stream)[8])(stream, &dynamic, &frames) == 0 && dynamic == 0 && frames == 480);
  CHECK(((BufferFn)VT(left)[3])(left, &buffer, &bytes) == 0 && buffer && bytes == 480 * 4 && buffer[bytes - 1] == 0);
  buffer[bytes - 1] = 1;
  CHECK(((NoArgFn)VT(stream)[9])(stream) == 0);
  CHECK(((NoArgFn)VT(stream)[6])(stream) == 0); /* Stop */
  CloseHandle(event);
}

static volatile int g_device_reports, g_reading_callbacks;
static volatile uint32_t g_callback_buttons;
static void* volatile g_reported_device;

typedef _Bool (*GiGamepadState)(void*, uint32_t*);
static void gi_device_callback(uint64_t token, void* context, void* device, uint64_t timestamp, uint32_t status,
                               uint32_t previous) {
  (void)token, (void)timestamp, (void)previous;
  if (context == (void*)7 && (status & 1)) {
    g_reported_device = device;
    g_device_reports++;
  }
}
static void gi_reading_callback(uint64_t token, void* context, void* reading, _Bool overrun) {
  (void)token, (void)context, (void)overrun;
  uint32_t state[7] = {0};
  if (((GiGamepadState)(*(void***)reading)[22])(reading, state)) g_callback_buttons |= state[0];
  g_reading_callbacks++;
}

/* GameInput: one gamepad. The test runs with --press A@0.05:1500, so A is
 * held from 50 ms after GameInputCreate. */
static void test_gameinput(void) {
  void* gi = 0;
  CHECK(GameInputCreate(&gi) == 0 && gi);
  void** vt = *(void***)gi;
  const uint64_t t1 = ((GiTimestamp)vt[3])(gi);
  Sleep(2);
  CHECK(((GiTimestamp)vt[3])(gi) > t1);
  void* reading = 0;
  CHECK(((GiCurrentReading)vt[4])(gi, 0x40000 /*Gamepad*/, 0, &reading) == 0 && reading);
  ((Release)(*(void***)reading)[2])(reading);
  reading = (void*)1;
  CHECK(((GiCurrentReading)vt[4])(gi, 0x10 /*Keyboard*/, 0, &reading) == (HRESULT)0x838A0003 && reading == 0);

  /* The device callback reports the gamepad during registration */
  uint64_t token = 0;
  CHECK(((GiRegisterDevice)vt[9])(gi, 0, 0x40000, 1, 2, (void*)7, (void*)gi_device_callback, &token) == 0 && token);
  CHECK(g_device_reports == 1 && g_reported_device);
  void* device = g_reported_device;
  typedef const uint8_t* (*InfoFn)(void*);
  typedef uint32_t (*StatusFn)(void*);
  const uint8_t* info = ((InfoFn)(*(void***)device)[3])(device);
  CHECK(info && *(uint32_t*)info == 320 && *(uint16_t*)(info + 4) == 0x045e && (*(uint32_t*)(info + 104) & 0x40000));
  CHECK(((StatusFn)(*(void***)device)[4])(device) & 1);
  CHECK(((GiRegisterDevice)vt[9])(gi, 0, 0x10, 1, 2, (void*)7, (void*)gi_device_callback, &token) == 0);
  CHECK(g_device_reports == 1); /* no keyboard */

  /* The scripted press reaches readings and reading callbacks */
  typedef HRESULT (*RegisterReadingFn)(void*, void*, uint32_t, float, void*, void*, uint64_t*);
  CHECK(((RegisterReadingFn)vt[8])(gi, 0, 0x40000, 0.0f, 0, (void*)gi_reading_callback, &token) == 0);
  uint32_t state[7] = {0};
  for (int i = 0; i < 400 && !(state[0] & 4); ++i) {
    Sleep(5);
    CHECK(((GiCurrentReading)vt[4])(gi, 0x40000, 0, &reading) == 0);
    CHECK(((GiGamepadState)(*(void***)reading)[22])(reading, state));
    ((Release)(*(void***)reading)[2])(reading);
  }
  CHECK(state[0] == 4); /* A */
  for (int i = 0; i < 200 && !g_reading_callbacks; ++i) Sleep(5);
  CHECK(g_reading_callbacks >= 1 && (g_callback_buttons & 4));

  void* dispatcher = 0;
  CHECK(((GiCreateDispatcher)vt[14])(gi, &dispatcher) == 0 && dispatcher);
  CHECK(!((GiDispatch)(*(void***)dispatcher)[3])(dispatcher, 0));
  ((Release)(*(void***)dispatcher)[2])(dispatcher);
  ((Release)vt[2])(gi);
}

/* ------------------------------------------------- DirectStorage probe */

static const uint8_t kFactoryIid[16] = {0x11, 0x9d, 0x95, 0x62};
static const uint8_t kQueueIid[16] = {0x22, 0x33};
typedef HRESULT (*ProbeCreate)(void*, const void*, const void*, void**);
typedef HRESULT (*ProbeSet)(void*, uint64_t);

/* zlib stream of "xbem DirectStorage inflate test! " x 8 (264 bytes) */
static const uint8_t kZlibChunk[48] = {
    'X', 'B', 'D', 'S', 0x78, 0xda, 0xab, 0x48, 0x4a, 0xcd, 0x55, 0x70, 0xc9, 0x2c, 0x4a, 0x4d,
    0x2e, 0x09, 0x2e, 0xc9, 0x2f, 0x4a, 0x4c, 0x4f, 0x55, 0xc8, 0xcc, 0x4b, 0xcb, 0x49, 0x2c, 0x49,
    0x55, 0x28, 0x49, 0x2d, 0x2e, 0x51, 0x54, 0xa8, 0x18, 0x19, 0x0a, 0x00, 0x6b, 0x8e, 0x61, 0x01};

/* Console DirectStorage: open this executable, read its header, then read
 * and inflate the zlib chunk above from the file, with a status array. */
/* DirectStorage work runs on the queue's own thread: wait for a status entry. */
typedef BOOL (*DsIsCompleteFn)(void*, uint32_t);
static int ds_wait(void* status, uint32_t index) {
  for (int i = 0; i < 5000; ++i) {
    if (((DsIsCompleteFn)(*(void***)status)[3])(status, index)) return 1;
    Sleep(1);
  }
  return 0;
}

static void test_dstorage(void) {
  void* factory = 0;
  CHECK(DStorageGetFactory(kFactoryIid, &factory) == 0 && factory);
  void** vt = *(void***)factory;
  typedef HRESULT (*OpenFileFn)(void*, const void*, const void*, void**);
  typedef HRESULT (*StatusFn)(void*, uint32_t, const char*, const void*, void**);
  char path[260];
  CHECK(GetModuleFileNameA(0, path, sizeof path) > 0);
  WCHAR wpath[260];
  int n = 0;
  for (; path[n]; ++n) wpath[n] = (WCHAR)path[n];
  wpath[n] = 0;
  void* file = 0;
  CHECK(((OpenFileFn)vt[4])(factory, wpath, kQueueIid, &file) == 0 && file);
  void* missing = (void*)1;
  CHECK(((OpenFileFn)vt[4])(factory, W("G:\\no-such-file.bin"), kQueueIid, &missing) < 0 && missing == 0);
  uint8_t info[52] = {0};
  typedef HRESULT (*InfoFn)(void*, void*);
  CHECK(((InfoFn)(*(void***)file)[4])(file, info) == 0 && *(uint32_t*)(info + 36) > 0x1000);
  void* status = 0;
  CHECK(((StatusFn)vt[5])(factory, 2, "test", kQueueIid, &status) == 0 && status);
  static const uint8_t desc[32] = {1, 2, 3};
  void* queue = 0;
  CHECK(((ProbeCreate)vt[10])(factory, desc, kQueueIid, &queue) == 0 && queue);
  CHECK(((ProbeSet)vt[8])(factory, 0x2000000) == 0); /* staging size: accepted */
  void** qv = *(void***)queue;
  typedef void (*EnqueueFn)(void*, const void*);
  typedef void (*EnqueueStatusFn)(void*, void*, uint32_t);
  typedef void (*SubmitFn)(void*);
  typedef BOOL (*IsCompleteFn)(void*, uint32_t);
  typedef HRESULT (*GetHrFn)(void*, uint32_t);
  CHECK(!((IsCompleteFn)(*(void***)status)[3])(status, 0));

  /* console request: options (compression), dest, 0, dest size, file, offset, size | uncompressed << 32 */
  static uint8_t header[64];
  uint64_t req[12] = {0, (uint64_t)(uintptr_t)header, 0, sizeof header, (uint64_t)(uintptr_t)file, 0, 64};
  ((EnqueueFn)qv[3])(queue, req);
  ((EnqueueStatusFn)qv[4])(queue, status, 0);
  ((SubmitFn)qv[6])(queue);
  CHECK(ds_wait(status, 0) && ((GetHrFn)(*(void***)status)[4])(status, 0) == 0);
  CHECK(header[0] == 'M' && header[1] == 'Z');

  /* Find the chunk in the file, then read it compressed. */
  static uint8_t image[0x40000];
  const uint32_t size = *(uint32_t*)(info + 36) < sizeof image ? *(uint32_t*)(info + 36) : sizeof image;
  uint64_t whole[12] = {0, (uint64_t)(uintptr_t)image, 0, size, (uint64_t)(uintptr_t)file, 0, size};
  ((EnqueueFn)qv[3])(queue, whole);
  ((EnqueueStatusFn)qv[4])(queue, status, 0);
  ((SubmitFn)qv[6])(queue);
  CHECK(ds_wait(status, 0));
  uint32_t at = 0;
  for (uint32_t i = 0; i + 4 <= size; ++i)
    if (image[i] == 'X' && image[i + 1] == 'B' && image[i + 2] == 'D' && image[i + 3] == 'S' && image[i + 4] == 0x78) {
      at = i + 4;
      break;
    }
  CHECK(at != 0);
  static uint8_t text[512];
  uint64_t zreq[12] = {1, (uint64_t)(uintptr_t)text, 0, sizeof text, (uint64_t)(uintptr_t)file, at,
                       44 | (uint64_t)264 << 32};
  ((EnqueueFn)qv[3])(queue, zreq);
  ((EnqueueStatusFn)qv[4])(queue, status, 1);
  ((SubmitFn)qv[6])(queue);
  CHECK(ds_wait(status, 1) && ((GetHrFn)(*(void***)status)[4])(status, 1) == 0);
  CHECK(text[0] == 'x' && text[5] == 'D' && text[263] == ' ' && text[264] == 0);
  /* A format the emulator cannot decode (2): delivered as zeros, reported done. */
  {
    static uint8_t unknown[64];
    memset(unknown, 0xee, sizeof unknown);
    uint64_t ureq[12] = {2, (uint64_t)(uintptr_t)unknown, 0, sizeof unknown, (uint64_t)(uintptr_t)file, 0, 32 | (uint64_t)64 << 32};
    ((EnqueueFn)qv[3])(queue, ureq);
    ((EnqueueStatusFn)qv[4])(queue, status, 1);
    ((SubmitFn)qv[6])(queue);
    CHECK(ds_wait(status, 1) && ((GetHrFn)(*(void***)status)[4])(status, 1) == 0);
    CHECK(unknown[0] == 0 && unknown[63] == 0);
  }
  /* A piece of a chunk: (base, offset, size) - the chunk covers the region at
   * base, and bytes [offset, offset + size) of its output go to base + offset
   * (the 264-byte text is 8 x 33 bytes); the rest of the region is untouched. */
  static uint8_t region[80];
  memset(region, 0xee, sizeof region);
  uint64_t preq[12] = {1, (uint64_t)(uintptr_t)region, 33, 33, (uint64_t)(uintptr_t)file, at, 44};
  ((EnqueueFn)qv[3])(queue, preq);
  ((EnqueueStatusFn)qv[4])(queue, status, 1);
  ((SubmitFn)qv[6])(queue);
  CHECK(ds_wait(status, 1) && ((GetHrFn)(*(void***)status)[4])(status, 1) == 0);
  CHECK(region[0] == 0xee && region[32] == 0xee && memcmp(region + 33, text + 33, 33) == 0 && region[66] == 0xee);
  /* Options 0x20xxxx: a physical destination - word 1 points to the frame
   * numbers of its 64 KiB pages, word 2 is the offset in it, word 3 the size;
   * bytes [offset, offset + size) of the chunk land in the pages. The frame
   * array itself is not written. */
  {
    uint64_t n = 1, frames[1] = {0};
    CHECK(XMemAllocatePhysicalPages(0, &n, frames) >= 0 && n == 1);
    uint8_t* view = XMemVirtualAlloc(0, 0x10000, MEM_RESERVE | 0x20000000, 0x3, PAGE_READWRITE);
    CHECK(view && XMemMapPhysicalPages(view, 1, frames) == view);
    memset(view, 0xee, 80);
    const uint64_t frame = frames[0];
    /* Enqueued with the frame array still empty: it is read at Submit. */
    uint64_t later[1] = {0};
    uint64_t phys[12] = {0x200001, (uint64_t)(uintptr_t)later, 33, 33, (uint64_t)(uintptr_t)file, at, 44};
    ((EnqueueFn)qv[3])(queue, phys);
    ((EnqueueStatusFn)qv[4])(queue, status, 1);
    CHECK(!((IsCompleteFn)(*(void***)status)[3])(status, 1));
    later[0] = frames[0];
    ((SubmitFn)qv[6])(queue);
    CHECK(ds_wait(status, 1) && ((GetHrFn)(*(void***)status)[4])(status, 1) == 0);
    CHECK(frames[0] == frame);
    CHECK(((volatile uint8_t*)view)[32] == 0xee && memcmp(view + 33, text + 33, 33) == 0 && view[66] == 0xee);
  }
  CHECK(*(volatile const uint8_t*)&kZlibChunk[47] == 0x01); /* keeps the chunk in the image */

  /* 12 EnqueueSetEvent(HANDLE): set once the work before it is done */
  typedef void (*SetEventFn)(void*, HANDLE);
  HANDLE done = CreateEventW(0, 1, 0, 0);
  ((SetEventFn)qv[12])(queue, done);
  ((SubmitFn)qv[6])(queue);
  CHECK(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);

  /* Coverage in the Submit log: 16 requests on a second file object, with a
   * gap of 0x7800 bytes that no request reads. */
  void* file2 = 0;
  CHECK(((OpenFileFn)vt[4])(factory, wpath, kQueueIid, &file2) == 0 && file2);
  /* Destinations in file order: the gap's memory (0x7800 bytes after the
   * eighth piece) is never written, and the log says it is all zero. */
  static uint8_t pieces[0x8800];
  for (int i = 0; i < 16; ++i) {
    const uint64_t offset = (i < 8 ? 0 : 0x7800) + (uint64_t)i * 0x100;
    uint64_t r[12] = {0, (uint64_t)(uintptr_t)(pieces + offset), 0, 0x100, (uint64_t)(uintptr_t)file2, offset, 0x100};
    ((EnqueueFn)qv[3])(queue, r);
  }
  ((EnqueueStatusFn)qv[4])(queue, status, 0);
  ((SubmitFn)qv[6])(queue);
  CHECK(ds_wait(status, 0)); /* the coverage log is written at the next Submit */
  ((SubmitFn)qv[6])(queue);
}

/* ------------------------------------------------- headless D3D12 */

typedef HRESULT (*CreateFenceFn)(void*, uint64_t, uint32_t, const void*, void**);
typedef HRESULT (*CreateQueueFn)(void*, const void*, const void*, void**);
typedef HRESULT (*QueueSignalFn)(void*, void*, uint64_t);
typedef uint64_t (*FenceValueFn)(void*);
typedef HRESULT (*FenceEventFn)(void*, uint64_t, HANDLE);
typedef HRESULT (*CreateCommittedFn)(void*, const void*, uint32_t, const void*, uint32_t, const void*, const void*, void**);
typedef HRESULT (*MapFn)(void*, uint32_t, const void*, void**);
typedef uint64_t (*GpuVaFn)(void*);
typedef HRESULT (*CreateComputePsoFn)(void*, const void*, const void*, void**);
typedef HRESULT (*CreateHeapFn)(void*, const void*, const void*, void**);
typedef uint64_t (*HeapStartFn)(void*);

static void test_headless_d3d12(void) {
  static const uint8_t iid[16] = {0x7d, 0x12};
  void* device = 0;
  CHECK(D3D12XboxCreateDevice(0, 0, iid, &device) == 0 && device);

  /* The title's DXGI path: device -> QueryInterface(IDXGIDevice) ->
   * GetAdapter(&adapter) -> EnumOutputs(0, &output), all via zeroed outputs. */
  typedef HRESULT (*QiFn)(void*, const void*, void**);
  typedef HRESULT (*OutFn)(void*, void**);
  typedef HRESULT (*IndexOutFn)(void*, uint32_t, void**);
  static const uint8_t dxgi_iid[16] = {0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
                                       0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c};
  void* same = 0;
  CHECK(((QiFn)VT(device)[0])(device, iid, &same) == 0 && same == device);
  void* dxgi = 0;
  CHECK(((QiFn)VT(device)[0])(device, dxgi_iid, &dxgi) == 0 && dxgi && dxgi != device);
  void* adapter = 0;
  CHECK(((OutFn)VT(dxgi)[7])(dxgi, &adapter) == 0 && adapter);
  void* output = 0;
  CHECK(((IndexOutFn)VT(adapter)[7])(adapter, 0, &output) == 0 && output);
  void* second = (void*)1;
  CHECK(((IndexOutFn)VT(adapter)[7])(adapter, 1, &second) == (HRESULT)0x887A0002 && second == 0);
  /* GetDisplayModeList: count, then 32-byte modes; the title looks for 120 Hz */
  typedef HRESULT (*ModesFn)(void*, uint32_t, uint32_t, uint32_t*, uint8_t*);
  uint32_t mode_count = 0;
  CHECK(((ModesFn)VT(output)[21])(output, 0, 0, &mode_count, 0) == 0 && mode_count >= 2);
  static uint8_t modes[8 * 32];
  CHECK(((ModesFn)VT(output)[21])(output, 0, 0, &mode_count, modes) == 0);
  int has120 = 0;
  for (uint32_t i = 0; i < mode_count; ++i) has120 |= *(uint32_t*)(modes + i * 32 + 8) == 120;
  CHECK(has120 && *(uint32_t*)modes == 3840);

  /* Methods without a typed implementation are logged probe slots, far past
   * the PC interface (the console device has many more methods). */
  typedef HRESULT (*AnyFn)(void*, uint64_t);
  CHECK(((AnyFn)VT(device)[200])(device, 0) == 0);
  CHECK(((AnyFn)VT(device)[300])(device, 0) == 0);
  /* An output variable zeroed on the stack receives a new object... */
  typedef HRESULT (*ProbeOutFn)(void*, uint64_t, void**);
  void* probe_out = 0;
  CHECK(((ProbeOutFn)VT(device)[301])(device, 0, &probe_out) == 0 && probe_out && probe_out != device);
  /* ...and a probe method reached with a foreign `this` is logged, not trusted. */
  static uint64_t foreign_this[8];
  CHECK(((AnyFn)VT(device)[302])(foreign_this, 1) == 0);

  void* fence = 0;
  CHECK(((CreateFenceFn)VT(device)[36])(device, 1, 0, iid, &fence) == 0 && fence);
  CHECK(((FenceValueFn)VT(fence)[8])(fence) == 1);
  static const uint32_t queue_desc[4] = {0};
  void* queue = 0;
  CHECK(((CreateQueueFn)VT(device)[8])(device, queue_desc, iid, &queue) == 0 && queue);
  HANDLE ev = CreateEventW(0, 1, 0, 0);
  CHECK(((FenceEventFn)VT(fence)[9])(fence, 5, ev) == 0);
  CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
  CHECK(((QueueSignalFn)VT(queue)[14])(queue, fence, 5) == 0); /* GPU work "completes" at once */
  CHECK(((FenceValueFn)VT(fence)[8])(fence) == 5);
  CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
  /* The console queue is shifted: slot 16 with a fence is Signal (and must not
   * write a timestamp frequency into the fence), a stack variable gets the
   * frequency. */
  void* const fence_vtable = *(void**)fence;
  CHECK(((QueueSignalFn)VT(queue)[16])(queue, fence, 9) == 0);
  CHECK(*(void**)fence == fence_vtable && ((FenceValueFn)VT(fence)[8])(fence) == 9);
  /* Console fence slot 12: the address of the fence value, read by the title */
  typedef const uint64_t* (*FenceAddrFn)(void*, uint64_t, void*, uint64_t);
  uint64_t fence_args[2] = {0x1234, 0x5678};
  const uint64_t* fence_value = ((FenceAddrFn)VT(fence)[12])(fence, 2, fence_args, 1);
  CHECK(fence_value && *fence_value == 9 && fence_args[0] == 0x1234);
  CHECK(((QueueSignalFn)VT(queue)[16])(queue, fence, 11) == 0 && *fence_value == 11);
  typedef HRESULT (*FreqFn)(void*, uint64_t*);
  uint64_t frequency = 0;
  CHECK(((FreqFn)VT(queue)[18])(queue, &frequency) == 0 && frequency == 100000000);

  /* D3D12_RESOURCE_DESC: buffer of 4096 bytes */
  uint8_t desc[56] = {0};
  desc[0] = 1;
  *(uint64_t*)(desc + 16) = 4096;
  void* buffer = 0;
  CHECK(((CreateCommittedFn)VT(device)[27])(device, 0, 0, desc, 0, 0, iid, &buffer) == 0 && buffer);
  uint8_t* mapped = 0;
  CHECK(((MapFn)VT(buffer)[8])(buffer, 0, 0, (void**)&mapped) == 0 && mapped);
  mapped[4095] = 0x42;
  /* unified memory: the GPU address is the CPU address */
  CHECK(((GpuVaFn)VT(buffer)[11])(buffer) == (uint64_t)(uintptr_t)mapped);

  /* D3D12_DESCRIPTOR_HEAP_DESC: 64 descriptors; D3D12.X returns handles in RAX */
  static const uint32_t heap_desc[4] = {0, 64, 1, 0};
  void* heap = 0;
  CHECK(((CreateHeapFn)VT(device)[14])(device, heap_desc, iid, &heap) == 0 && heap);
  const uint64_t start = ((HeapStartFn)VT(heap)[9])(heap);
  CHECK(start != 0);
  const uint64_t gpu_start = ((HeapStartFn)VT(heap)[10])(heap);
  CHECK(gpu_start == start);
  *(volatile uint64_t*)(uintptr_t)(gpu_start + 63 * 32) = 1; /* descriptors are writable memory */
  /* Console desc: the memory the title set aside for the descriptors and its
   * size follow the PC fields */
  uint8_t* title_mem = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  uint64_t heap_desc_x[4] = {(uint64_t)64 << 32, 1, (uint64_t)(uintptr_t)title_mem, 64 * 32};
  void* heap_x = 0;
  CHECK(((CreateHeapFn)VT(device)[14])(device, heap_desc_x, iid, &heap_x) == 0 && heap_x);
  CHECK(((HeapStartFn)VT(heap_x)[9])(heap_x) == (uint64_t)(uintptr_t)title_mem);
  /* Only the 16 PC bytes set, the rest stack contents (fn 0x1c744f0 of the
   * title): a pointer to allocated memory there is not the heap's memory */
  uint64_t heap_desc_pc[4] = {(uint64_t)64 << 32, 1, (uint64_t)(uintptr_t)title_mem, (uint64_t)(uintptr_t)title_mem};
  void* heap_pc = 0;
  CHECK(((CreateHeapFn)VT(device)[14])(device, heap_desc_pc, iid, &heap_pc) == 0 && heap_pc);
  const uint64_t pc_start = ((HeapStartFn)VT(heap_pc)[9])(heap_pc);
  CHECK(pc_start && pc_start != (uint64_t)(uintptr_t)title_mem);
  uint64_t heap_desc_small[4] = {(uint64_t)64 << 32, 1, (uint64_t)(uintptr_t)title_mem, 32};
  void* heap_small = 0;
  CHECK(((CreateHeapFn)VT(device)[14])(device, heap_desc_small, iid, &heap_small) == 0 && heap_small);
  CHECK(((HeapStartFn)VT(heap_small)[9])(heap_small) != (uint64_t)(uintptr_t)title_mem);
  /* Stack contents that look like (pointer, size): a pointer into this
   * thread's stack is never the heap's memory (GTA VI's render-thread heaps
   * once landed on the main thread's stack top this way). */
  {
    volatile uint8_t on_stack[64 * 32];
    on_stack[0] = 1;
    uint64_t heap_desc_stack[4] = {(uint64_t)64 << 32, 1, (uint64_t)(uintptr_t)on_stack, 0x2600000};
    void* heap_stack = 0;
    CHECK(((CreateHeapFn)VT(device)[14])(device, heap_desc_stack, iid, &heap_stack) == 0 && heap_stack);
    const uint64_t stack_start = ((HeapStartFn)VT(heap_stack)[9])(heap_stack);
    CHECK(stack_start && stack_start != (uint64_t)(uintptr_t)on_stack);
  }

  /* Heaps: CreateHeap (28); placed resources live at heap + offset. */
  typedef HRESULT (*CreateMemHeapFn)(void*, const void*, const void*, void**);
  typedef HRESULT (*PlacedXFn)(void*, void*, const void*, uint64_t, uint64_t, const void*, void**);
  typedef HRESULT (*PlacedFn)(void*, void*, uint64_t, const void*, uint32_t, const void*, const void*, void**);
  static const uint8_t heap_iid[16] = {0x02, 0x25, 0x3b, 0x6b, 0x51, 0x6e, 0xb3, 0x45,
                                       0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3};
  static const uint8_t resource_iid[16] = {0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7, 0x59, 0x40,
                                           0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad};
  uint64_t mem_heap_desc[5] = {0x100000, 0, 0, 0, 0};
  void* mem_heap = 0;
  CHECK(((CreateMemHeapFn)VT(device)[28])(device, mem_heap_desc, heap_iid, &mem_heap) == 0 && mem_heap);
  void* placed = 0;
  CHECK(((PlacedFn)VT(device)[29])(device, mem_heap, 0x2000, desc, 0, 0, iid, &placed) == 0 && placed);
  *(volatile uint8_t*)(uintptr_t)((GpuVaFn)VT(placed)[11])(placed) = 7;

  /* 133 CreatePlacedResourceX(va, desc, layout, 0, riid, ppv): a buffer on the title's own pages */
  uint8_t* own = VirtualAlloc(0, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  void* at_va = 0;
  CHECK(((PlacedXFn)VT(device)[133])(device, own + 0x10000, desc, 0xffffffff, 0, resource_iid, &at_va) == 0 && at_va);
  CHECK(((GpuVaFn)VT(at_va)[11])(at_va) == (uint64_t)(uintptr_t)(own + 0x10000));

  /* Structure returns: result buffer in RCX and the object in RDX (console
   * form), or the PC form - both are answered. */
  typedef uint64_t* (*AllocInfoFn)(uint64_t*, void*, uint32_t, uint32_t, const void*);
  typedef uint8_t* (*GetDescFn)(void*, void*);
  uint64_t info[2] = {0, 0};
  CHECK(((AllocInfoFn)VT(device)[25])(info, device, 1, 1, desc) == info);
  CHECK(info[0] == 0x10000 && info[1] == 0x10000); /* 4 KiB buffer -> one 64 KiB page */
  info[0] = info[1] = 0; /* 108: the console variant, same call shape */
  CHECK(((AllocInfoFn)VT(device)[108])(info, device, 1, 1, desc) == info && info[0] == 0x10000);
  /* 3840x2160 R8G8B8A8 render target, 1 mip: 15360-byte rows x 2160, in 64 KiB pages */
  uint8_t tex[56] = {0};
  tex[0] = 3; /* TEXTURE2D */
  *(uint64_t*)(tex + 16) = 3840;
  *(uint32_t*)(tex + 24) = 2160;
  *(uint16_t*)(tex + 28) = 1;
  *(uint16_t*)(tex + 30) = 1;
  *(uint32_t*)(tex + 32) = 28; /* R8G8B8A8_UNORM */
  *(uint32_t*)(tex + 36) = 1;
  CHECK(((AllocInfoFn)VT(device)[25])(info, device, 1, 1, tex) == info && info[0] == 0x1FB0000);
  /* BC1 256x256 with a full mip chain: 32768 + 8192 + 2048 + 512 + 6 x 512 (small mips) bytes */
  *(uint64_t*)(tex + 16) = 256;
  *(uint32_t*)(tex + 24) = 256;
  *(uint16_t*)(tex + 30) = 0;
  *(uint32_t*)(tex + 32) = 71; /* BC1_UNORM */
  CHECK(((AllocInfoFn)VT(device)[25])(info, device, 1, 1, tex) == info && info[0] == 0x10000);
  /* The same texture asking for 4 KiB alignment (a small resource, as GTA VI
   * asks for its small textures): size in 4 KiB units, 4 KiB alignment. */
  *(uint64_t*)(tex + 8) = 0x1000;
  CHECK(((AllocInfoFn)VT(device)[25])(info, device, 1, 1, tex) == info && info[0] == 0xD000 && info[1] == 0x1000);
  *(uint64_t*)(tex + 8) = 0;
  uint8_t got[56] = {0};
  CHECK(((GetDescFn)VT(buffer)[10])(got, buffer) == got && got[0] == 1 && *(uint64_t*)(got + 16) == 4096);
  uint8_t got_pc[56] = {0};
  CHECK(((GetDescFn)VT(buffer)[10])(buffer, got_pc) == got_pc && memcmp(got, got_pc, 56) == 0);

  /* 127: CreateCommittedResource3 - heap properties, heap flags, desc, initial
   * layout, clear value, protected session, castable format count/array, riid, ppv */
  typedef HRESULT (*CreateCommitted3Fn)(void*, const void*, uint32_t, const void*, uint32_t, const void*, void*,
                                        uint32_t, const void*, const void*, void**);
  static const uint32_t heap_props[5] = {1, 0, 0, 1, 1};
  *(uint64_t*)(tex + 16) = 3840;
  *(uint32_t*)(tex + 24) = 2160;
  *(uint16_t*)(tex + 30) = 1;
  *(uint32_t*)(tex + 32) = 28;
  void* back_buffer = 0;
  CHECK(((CreateCommitted3Fn)VT(device)[127])(device, heap_props, 0x8, tex, 2, 0, 0, 0, 0, resource_iid,
                                              &back_buffer) == 0 && back_buffer);
  CHECK(((GpuVaFn)VT(back_buffer)[11])(back_buffer) != 0);

  /* Descriptors are memory: CreateRenderTargetView (20) / CreateShaderResourceView
   * (18) write the emulator's 32-byte encoding ("XBDS", kind, resource) at the
   * handle; copies by the title and CopyDescriptorsSimple (24) carry it. */
  typedef void (*CreateViewFn)(void*, void*, const void*, uint64_t);
  typedef void (*CopySimpleFn)(void*, uint32_t, uint64_t, uint64_t, uint32_t);
  ((CreateViewFn)VT(device)[20])(device, back_buffer, 0, start + 2 * 32);
  ((CreateViewFn)VT(device)[18])(device, buffer, 0, start + 3 * 32);
  const uint32_t* rtv = (const uint32_t*)(uintptr_t)(start + 2 * 32);
  CHECK(rtv[0] == 0x53444258 && ((const uint8_t*)rtv)[4] == 4 && *(void* const*)(rtv + 2) == back_buffer);
  ((CopySimpleFn)VT(device)[24])(device, 2, start + 10 * 32, start + 2 * 32, 0);
  const uint32_t* copied = (const uint32_t*)(uintptr_t)(start + 11 * 32);
  CHECK(memcmp((const void*)(uintptr_t)(start + 10 * 32), rtv, 32) == 0);
  CHECK(copied[0] == 0x53444258 && ((const uint8_t*)copied)[4] == 2 && *(void* const*)(copied + 2) == buffer);
  /* 76: CreateCommittedResource's seven arguments */
  void* texture76 = 0;
  CHECK(((CreateCommittedFn)VT(device)[76])(device, heap_props, 0, tex, 0, 0, resource_iid, &texture76) == 0 &&
        texture76 && texture76 != back_buffer);

  /* Root signatures, as XboxBvhBuilder makes them: a 1.1 description with a
   * 32-bit constants parameter and a UAV descriptor -> serialized blob ->
   * CreateRootSignature (16) and the deserializer. */
  struct { uint32_t type, pad; uint64_t a, b; uint32_t vis, pad2; } rs_params[2] = {
      {1, 0, 0, 6, 0, 0},           /* constants b0, 6 values */
      {4, 0, 0, 0, 0, 0}};          /* UAV u0 */
  struct { uint32_t version, pad, num_params, pad0; uint64_t params; uint32_t num_samplers, pad1; uint64_t samplers;
           uint32_t flags, pad2; } rs_desc = {2, 0, 2, 0, (uint64_t)(uintptr_t)rs_params, 0, 0, 0, 0x03000000, 0};
  typedef void* (*BlobPtrFn)(void*);
  typedef size_t (*BlobSizeFn)(void*);
  void* rs_blob = 0;
  void* rs_error = (void*)1;
  CHECK(D3D12SerializeVersionedRootSignature(&rs_desc, &rs_blob, &rs_error) == 0 && rs_blob && rs_error == 0);
  const uint8_t* rs_bytes = ((BlobPtrFn)VT(rs_blob)[3])(rs_blob);
  const size_t rs_size = ((BlobSizeFn)VT(rs_blob)[4])(rs_blob);
  CHECK(rs_size > 40 && memcmp(rs_bytes, "DXBC", 4) == 0);
  typedef HRESULT (*CreateRsFn)(void*, uint32_t, const void*, size_t, const void*, void**);
  void* root_sig = 0;
  CHECK(((CreateRsFn)VT(device)[16])(device, 0, rs_bytes, rs_size, iid, &root_sig) == 0 && root_sig);
  void* deser = 0;
  CHECK(D3D12CreateRootSignatureDeserializer(rs_bytes, rs_size, iid, &deser) == 0 && deser);
  typedef const uint32_t* (*GetRsDescFn)(void*);
  const uint32_t* rs_back = ((GetRsDescFn)VT(deser)[3])(deser);
  CHECK(rs_back && rs_back[0] == 2 && rs_back[8] == 0x03000000);

  /* 86 CreatePipelineState(D3D12_PIPELINE_STATE_STREAM_DESC, riid, ppv): a
   * PC pipeline state stream (GTA VI's carry DXIL clear shaders); the object
   * is created and the stream logged. Subobjects: root signature, VS. */
  static const uint8_t dxil_stub[8] = {'D', 'X', 'B', 'C'};
  struct { uint64_t type_rs; void* rs; uint64_t type_vs; const void* code; uint64_t size; } stream = {
      0, root_sig, 1, dxil_stub, sizeof dxil_stub};
  struct { uint64_t size; void* data; } stream_desc = {sizeof stream, &stream};
  typedef HRESULT (*CreateStreamPsoFn)(void*, const void*, const void*, void**);
  void* stream_pso = 0;
  CHECK(((CreateStreamPsoFn)VT(device)[86])(device, &stream_desc, iid, &stream_pso) == 0 && stream_pso);

  /* Frame pacing (78-80): accepted; WaitFrameEventX hands out fresh tokens */
  typedef HRESULT (*Frame4Fn)(void*, uint64_t, uint64_t, uint64_t, uint64_t);
  typedef HRESULT (*WaitFn)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t*);
  CHECK(((Frame4Fn)VT(device)[78])(device, 0, 33333, 3, 0) == 0);
  CHECK(((Frame4Fn)VT(device)[79])(device, 0, 0, 0, 0) == 0);
  uint64_t token1 = 0, token2 = 0;
  CHECK(((WaitFn)VT(device)[80])(device, 0, 100, 0, 0, &token1) == 0 && token1 != 0);
  CHECK(((WaitFn)VT(device)[80])(device, 0, 100, 0, 0, &token2) == 0 && token2 != token1);

  /* Command lists: 73 CreateCommandListX(desc, allocator, state, riid, ppv)
   * and the PC 12 CreateCommandList. The title writes commands through the
   * list's cursor (+0x18) while it is below the limits (+0x28 dispatch, +0x30
   * other commands), as the title does at +0x3258a; at the limit it calls the
   * list instead, so the emulator sees every command. */
  typedef HRESULT (*CreateListXFn)(void*, const void*, void*, void*, const void*, void**);
  typedef HRESULT (*CreateListFn)(void*, uint32_t, uint32_t, void*, void*, const void*, void**);
  typedef HRESULT (*TopologyFn)(void*, uint32_t);
  static const uint32_t list_desc[4] = {0, 0, 0, 0}; /* DIRECT */
  void* list = 0;
  CHECK(((CreateListXFn)VT(device)[73])(device, list_desc, 0, 0, iid, &list) == 0 && list);
  uint8_t* lb = (uint8_t*)list;
  uint64_t* cursor = (uint64_t*)(lb + 0x18);
  const uint64_t limit = *(uint64_t*)(lb + 0x30);
  CHECK(*cursor != 0 && *cursor == limit && *(uint64_t*)(lb + 0x28) == limit);
  int inline_writes = 0;
  if (*cursor < limit) { /* the title's fast path, never taken */
    *(uint32_t*)(uintptr_t)*cursor = 0xc000b600;
    *cursor += 8;
    ++inline_writes;
  }
  CHECK(inline_writes == 0 && *cursor == limit);
  /* the same cursor bound with a size, as dispatches check it */
  CHECK(!(*cursor + 16 <= *(uint64_t*)(lb + 0x28)));
  /* an unchecked write would land in memory, not fault */
  *(volatile uint32_t*)(uintptr_t)*cursor = 0;
  /* the console fields the title touches start zeroed and are writable */
  CHECK(lb[0x140] == 0 && lb[0x143] == 0 && *(uint32_t*)(lb + 0x544) == 0);
  lb[0x140] = 1;
  lb[0x141] = 1;
  CHECK(((TopologyFn)VT(list)[20])(list, 0x11) == 0); /* IASetPrimitiveTopology(RECTLIST) */
  /* Recorded and decoded for --capture-commands (the test runs with a
   * capture of its whole run): a render target, a draw, Close, then the
   * console's ExecuteCommandListsX (queue slot 35) and PresentX (32). */
  typedef void (*OmSetFn)(void*, uint32_t, const uint64_t*, int, const uint64_t*);
  typedef void (*DrawFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
  typedef HRESULT (*CloseFn)(void*);
  typedef uint64_t (*ExecuteXFn)(void*, uint64_t, void* const*, uint64_t);
  const uint64_t rtv_handle = start + 2 * 32;
  ((OmSetFn)VT(list)[50])(list, 1, &rtv_handle, 0, 0);
  ((DrawFn)VT(list)[12])(list, 3, 1, 0, 0);
  /* 148: transient memory for the list (XboxBvhBuilder's upload buffers):
   * writable, aligned, below 2^40, distinct allocations */
  {
    typedef uint8_t* (*TransientFn)(void*, uint64_t, uint64_t);
    uint8_t* t1 = ((TransientFn)VT(list)[148])(list, 0x280, 0x10);
    uint8_t* t2 = ((TransientFn)VT(list)[148])(list, 0x100, 0x100);
    CHECK(t1 && t2 && ((uintptr_t)t1 & 0xf) == 0 && ((uintptr_t)t2 & 0xff) == 0);
    CHECK(t2 >= t1 + 0x280 && (uint64_t)(uintptr_t)t2 < (1ull << 40));
    t1[0x27f] = 1;
    t2[0xff] = 2;
    CHECK(t1[0x27f] == 1 && ((TransientFn)VT(list)[148])(list, 0, 0x10) == 0);
  }
  /* GPU memory writes the list makes when it executes (as GTA VI's frame
   * labels): 71 (address, 32-bit value, 0, 0) and 72 (address, 0, 0, 1) - a
   * 64-bit timestamp. Nothing is written while recording. */
  typedef void (*LabelFn)(void*, void*, uint64_t, uint64_t, uint32_t);
  static volatile uint32_t label[2] = {0, 0};
  static volatile uint64_t gpu_time = 0;
  ((LabelFn)VT(list)[71])(list, (void*)&label[1], 0x29a5ee15, 0, 0);
  ((LabelFn)VT(list)[72])(list, (void*)&gpu_time, 0, 0, 1);
  CHECK(label[1] == 0 && gpu_time == 0);
  CHECK(((CloseFn)VT(list)[9])(list) == 0);
  void* const lists[1] = {list};
  ((ExecuteXFn)VT(queue)[35])(queue, 1, lists, 0);
  CHECK(label[1] == 0x29a5ee15 && label[0] == 0 && gpu_time != 0);
  void* pc_list = 0;
  CHECK(((CreateListFn)VT(device)[12])(device, 0, 0, 0, 0, iid, &pc_list) == 0 && pc_list && pc_list != list);
  CHECK(*(uint64_t*)((uint8_t*)pc_list + 0x18) == *(uint64_t*)((uint8_t*)pc_list + 0x30));

  /* Compute pipeline: the shader binary is captured (--dump-shaders) */
  static const uint8_t shader[32] = {'X', 'B', 'E', 'M', 's', 'h', 'a', 'd', 'e', 'r'};
  const void* pso_desc[3] = {0, shader, (void*)(uintptr_t)sizeof shader};
  void* pso = 0;
  CHECK(((CreateComputePsoFn)VT(device)[11])(device, pso_desc, iid, &pso) == 0 && pso);
}

DWORD guest_main(void* peb) {
  (void)peb;
  test_xmem();
  test_zen2_instructions();
  test_xg();
  test_headless_d3d12();
  test_dstorage();
  test_gameinput();
  test_audio();
  test_sandbox();
  test_gdk_async_user();
  test_audio_endpoint();
  test_locale();
  test_time();
  test_topology();
  test_resources();
  test_com();
  test_threads();
  test_network();
  test_misc();
  return (DWORD)finish("console");
}
