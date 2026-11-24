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
}

typedef void** Vt;
#define VT(o) (*(Vt*)(o))

/* ------------------------------------------------------------------- XG */

/* Texture uploads: the computer copies linear rows into the emulator's
 * linear D3D12 layout (rows aligned to 256 bytes, subresources to 512). */
static void test_xg(void) {
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

static void test_gameinput(void) {
  void* gi = 0;
  CHECK(GameInputCreate(&gi) == 0 && gi);
  void** vt = *(void***)gi;
  const uint64_t t1 = ((GiTimestamp)vt[3])(gi);
  Sleep(2);
  CHECK(((GiTimestamp)vt[3])(gi) > t1);
  void* reading = (void*)1;
  CHECK(((GiCurrentReading)vt[4])(gi, 0x40000 /*Gamepad*/, 0, &reading) == (HRESULT)0x838A0003 && reading == 0);
  uint64_t token = 0;
  CHECK(((GiRegisterDevice)vt[9])(gi, 0, 0x40000, 1, 1, 0, 0, &token) == 0 && token != 0);
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
  CHECK(((IsCompleteFn)(*(void***)status)[3])(status, 0) && ((GetHrFn)(*(void***)status)[4])(status, 0) == 0);
  CHECK(header[0] == 'M' && header[1] == 'Z');

  /* Find the chunk in the file, then read it compressed. */
  static uint8_t image[0x40000];
  const uint32_t size = *(uint32_t*)(info + 36) < sizeof image ? *(uint32_t*)(info + 36) : sizeof image;
  uint64_t whole[12] = {0, (uint64_t)(uintptr_t)image, 0, size, (uint64_t)(uintptr_t)file, 0, size};
  ((EnqueueFn)qv[3])(queue, whole);
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
  CHECK(((IsCompleteFn)(*(void***)status)[3])(status, 1) && ((GetHrFn)(*(void***)status)[4])(status, 1) == 0);
  CHECK(text[0] == 'x' && text[5] == 'D' && text[263] == ' ' && text[264] == 0);
  CHECK(*(volatile const uint8_t*)&kZlibChunk[47] == 0x01); /* keeps the chunk in the image */

  /* 12 EnqueueSetEvent(HANDLE): requests complete at once, so the event is set */
  typedef void (*SetEventFn)(void*, HANDLE);
  HANDLE done = CreateEventW(0, 1, 0, 0);
  ((SetEventFn)qv[12])(queue, done);
  CHECK(WaitForSingleObject(done, 0) == WAIT_OBJECT_0);

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
  /* Console desc: memory the title set aside for the descriptors follows the PC fields */
  uint8_t* title_mem = VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  uint64_t heap_desc_x[4] = {(uint64_t)64 << 32, 1, (uint64_t)(uintptr_t)title_mem, 0};
  void* heap_x = 0;
  CHECK(((CreateHeapFn)VT(device)[14])(device, heap_desc_x, iid, &heap_x) == 0 && heap_x);
  CHECK(((HeapStartFn)VT(heap_x)[9])(heap_x) == (uint64_t)(uintptr_t)title_mem);

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
