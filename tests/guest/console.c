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

DWORD guest_main(void* peb) {
  (void)peb;
  test_xmem();
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
