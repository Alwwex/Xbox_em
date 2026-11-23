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
IMP BOOL XMemIsToolsMemAvailable(void);

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
  CHECK(XMemMapPhysicalPages(va, 1, 0) == va); /* unmap */
  CHECK(XMemFreePhysicalPages(1, pages) >= 0);
  CHECK(VirtualFree(va, 0, MEM_RELEASE));

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

static void test_dstorage_probe(void) {
  void* factory = 0;
  CHECK(DStorageGetFactory(kFactoryIid, &factory) == 0 && factory);
  void** vt = *(void***)factory;
  void* queue = 0;
  static const uint8_t desc[32] = {1, 2, 3};
  CHECK(((ProbeCreate)vt[3])(factory, desc, kQueueIid, &queue) == 0 && queue && queue != factory);
  CHECK(((ProbeSet)vt[8])(factory, 0x2000000) == 0);
  CHECK(((ProbeSet)(*(void***)queue)[5])(queue, 7) == 0);
  /* An output variable zeroed on the stack receives a new object. */
  typedef HRESULT (*ProbeOut)(void*, uint64_t, void**);
  void* out = 0;
  CHECK(((ProbeOut)vt[20])(factory, 0, &out) == 0 && out && out != factory);
  /* A probe method reached with a foreign `this` is logged, not trusted. */
  static uint64_t foreign[8];
  CHECK(((ProbeSet)vt[5])(foreign, 1) == 0);
}

/* ------------------------------------------------- headless D3D12 */

typedef void** Vt;
#define VT(o) (*(Vt*)(o))
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
  CHECK(((AnyFn)VT(device)[26])(device, 0) == 0);
  CHECK(((AnyFn)VT(device)[300])(device, 0) == 0);

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

  /* Heaps: CreateHeap (28) and the console's heap over already-mapped memory
   * (133: address, ..., riid, ppv); placed resources live at heap + offset. */
  typedef HRESULT (*CreateMemHeapFn)(void*, const void*, const void*, void**);
  typedef HRESULT (*HeapAtFn)(void*, void*, uint64_t, uint64_t, uint64_t, const void*, void**);
  typedef HRESULT (*PlacedFn)(void*, void*, uint64_t, const void*, uint32_t, const void*, const void*, void**);
  static const uint8_t heap_iid[16] = {0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7, 0x59, 0x40,
                                       0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad};
  uint64_t mem_heap_desc[5] = {0x100000, 0, 0, 0, 0};
  void* mem_heap = 0;
  CHECK(((CreateMemHeapFn)VT(device)[28])(device, mem_heap_desc, heap_iid, &mem_heap) == 0 && mem_heap);
  uint8_t* own = VirtualAlloc(0, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  void* at_heap = 0;
  CHECK(((HeapAtFn)VT(device)[133])(device, own, 0, 0xffffffff, 0, heap_iid, &at_heap) == 0 && at_heap);
  void* placed = 0;
  CHECK(((PlacedFn)VT(device)[29])(device, at_heap, 0x10000, desc, 0, 0, iid, &placed) == 0 && placed);
  CHECK(((GpuVaFn)VT(placed)[11])(placed) == (uint64_t)(uintptr_t)(own + 0x10000));
  CHECK(((PlacedFn)VT(device)[29])(device, mem_heap, 0x2000, desc, 0, 0, iid, &placed) == 0 && placed);
  *(volatile uint8_t*)(uintptr_t)((GpuVaFn)VT(placed)[11])(placed) = 7;

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
  test_headless_d3d12();
  test_dstorage_probe();
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
