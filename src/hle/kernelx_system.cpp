// kernelx / advapi32-level system services a title touches at start-up:
// the registry (empty on a console), ETW event providers (logging only),
// COM basics, WinRT strings and activation, WinRT error reporting and the
// random number APIs. Also defines the small oleaut32 (BSTR) and bcrypt
// (random numbers) modules.

#include <algorithm>
#include <atomic>
#include <new>
#include <random>

#include "hle/heap.h"
#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;

constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002);
constexpr HRESULT REGDB_E_CLASSNOTREG = static_cast<HRESULT>(0x80040154);
constexpr HRESULT CLASS_E_CLASSNOTAVAILABLE = static_cast<HRESULT>(0x80040111);

void fill_random(void* out, size_t n) {
  static thread_local std::mt19937_64 gen{std::random_device{}()};
  auto* p = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(gen());
}

std::string guid_text(const void* g) {
  if (!g) return "(null)";
  return guid_to_string(static_cast<const uint8_t*>(g));
}

// ---------------------------------------------------------------- registry
// A console title has no registry to speak of. Opening any key fails with
// ERROR_FILE_NOT_FOUND, which games handle as "use the default".

LONG registry_missing(const char* fn, const std::string& what) {
  XBEM_DEBUG("kernelx", "{}({}) -> ERROR_FILE_NOT_FOUND (no registry)", fn, what);
  return ERROR_FILE_NOT_FOUND;
}

LONG XBEM_GUEST_ABI hle_RegOpenKeyW(HANDLE, LPCWSTR sub, HANDLE* out) {
  if (out) *out = 0;
  return registry_missing("RegOpenKey", narrow(sub));
}
LONG XBEM_GUEST_ABI hle_RegOpenKeyA(HANDLE, LPCSTR sub, HANDLE* out) {
  if (out) *out = 0;
  return registry_missing("RegOpenKey", narrow(sub));
}
LONG XBEM_GUEST_ABI hle_RegOpenKeyExW(HANDLE, LPCWSTR sub, DWORD, DWORD, HANDLE* out) {
  if (out) *out = 0;
  return registry_missing("RegOpenKeyEx", narrow(sub));
}
LONG XBEM_GUEST_ABI hle_RegOpenKeyExA(HANDLE, LPCSTR sub, DWORD, DWORD, HANDLE* out) {
  if (out) *out = 0;
  return registry_missing("RegOpenKeyEx", narrow(sub));
}
LONG XBEM_GUEST_ABI hle_RegCreateKeyExW(HANDLE, LPCWSTR sub, DWORD, void*, DWORD, DWORD, void*, HANDLE* out, DWORD*) {
  if (out) *out = 0;
  XBEM_DEBUG("kernelx", "RegCreateKeyEx({}) -> ERROR_ACCESS_DENIED", narrow(sub));
  return ERROR_ACCESS_DENIED;
}
LONG XBEM_GUEST_ABI hle_RegQueryValueExW(HANDLE, LPCWSTR name, DWORD*, DWORD*, void*, DWORD*) {
  return registry_missing("RegQueryValueEx", narrow(name));
}
LONG XBEM_GUEST_ABI hle_RegQueryValueExA(HANDLE, LPCSTR name, DWORD*, DWORD*, void*, DWORD*) {
  return registry_missing("RegQueryValueEx", narrow(name));
}
LONG XBEM_GUEST_ABI hle_RegGetValueW(HANDLE, LPCWSTR sub, LPCWSTR value, DWORD, DWORD*, void*, DWORD*) {
  return registry_missing("RegGetValue", narrow(sub) + "\\" + narrow(value));
}
LONG XBEM_GUEST_ABI hle_RegSetValueExW(HANDLE, LPCWSTR, DWORD, DWORD, const void*, DWORD) { return ERROR_ACCESS_DENIED; }
LONG XBEM_GUEST_ABI hle_RegDeleteValueW(HANDLE, LPCWSTR) { return ERROR_FILE_NOT_FOUND; }
LONG XBEM_GUEST_ABI hle_RegEnumKeyExW(HANDLE, DWORD, LPWSTR, DWORD*, DWORD*, LPWSTR, DWORD*, void*) { return 259; }  // ERROR_NO_MORE_ITEMS
LONG XBEM_GUEST_ABI hle_RegEnumValueW(HANDLE, DWORD, LPWSTR, DWORD*, DWORD*, DWORD*, void*, DWORD*) { return 259; }
LONG XBEM_GUEST_ABI hle_RegQueryInfoKeyW(HANDLE, LPWSTR, DWORD*, DWORD*, DWORD*, DWORD*, DWORD*, DWORD*, DWORD*, DWORD*,
                                          DWORD*, void*) {
  return ERROR_INVALID_HANDLE;
}
LONG XBEM_GUEST_ABI hle_RegCloseKey(HANDLE) { return ERROR_SUCCESS; }
LONG XBEM_GUEST_ABI hle_RegNotifyChangeKeyValue(HANDLE, BOOL, DWORD, HANDLE, BOOL) { return ERROR_INVALID_HANDLE; }

// --------------------------------------------------------------------- ETW
// Event providers register successfully and every event is dropped.

std::atomic<uint64_t> g_next_reghandle{0x1000};

ULONG XBEM_GUEST_ABI hle_EventRegister(const void* provider, uint64_t, void*, uint64_t* handle) {
  if (handle) *handle = g_next_reghandle++;
  XBEM_DEBUG("kernelx", "EventRegister({}) - events are discarded", guid_text(provider));
  return ERROR_SUCCESS;
}
ULONG XBEM_GUEST_ABI hle_EventUnregister(uint64_t) { return ERROR_SUCCESS; }
ULONG XBEM_GUEST_ABI hle_EventSetInformation(uint64_t, DWORD, void*, ULONG) { return ERROR_SUCCESS; }
ULONG XBEM_GUEST_ABI hle_EventWrite(uint64_t, const void*, ULONG, void*) { return ERROR_SUCCESS; }
ULONG XBEM_GUEST_ABI hle_EventWriteTransfer(uint64_t, const void*, const void*, const void*, ULONG, void*) {
  return ERROR_SUCCESS;
}
ULONG XBEM_GUEST_ABI hle_EventWriteEx(uint64_t, const void*, uint64_t, ULONG, const void*, const void*, ULONG, void*) {
  return ERROR_SUCCESS;
}
ULONG XBEM_GUEST_ABI hle_EventWriteString(uint64_t, BYTE, ULONGLONG, LPCWSTR) { return ERROR_SUCCESS; }
BOOLEAN XBEM_GUEST_ABI hle_EventEnabled(uint64_t, const void*) { return 0; }
BOOLEAN XBEM_GUEST_ABI hle_EventProviderEnabled(uint64_t, BYTE, ULONGLONG) { return 0; }
ULONG XBEM_GUEST_ABI hle_EventActivityIdControl(ULONG, void* id) {
  if (id) std::memset(id, 0, 16);
  return ERROR_SUCCESS;
}
// Classic (MOF) tracing.
ULONG XBEM_GUEST_ABI hle_RegisterTraceGuidsW(uint64_t, void*, const void*, ULONG, void*, LPCWSTR, LPCWSTR,
                                              uint64_t* handle) {
  if (handle) *handle = g_next_reghandle++;
  return ERROR_SUCCESS;
}
ULONG XBEM_GUEST_ABI hle_UnregisterTraceGuids(uint64_t) { return ERROR_SUCCESS; }
uint64_t XBEM_GUEST_ABI hle_GetTraceLoggerHandle(void*) { return 0; }
UINT XBEM_GUEST_ABI hle_GetTraceEnableLevel(uint64_t) { return 0; }
ULONG XBEM_GUEST_ABI hle_GetTraceEnableFlags(uint64_t) { return 0; }
ULONG XBEM_GUEST_ABI hle_TraceEvent(uint64_t, void*) { return ERROR_SUCCESS; }
ULONG XBEM_GUEST_ABI hle_TraceMessage(uint64_t, ULONG, const void*, uint16_t, ...) { return ERROR_SUCCESS; }

// ------------------------------------------------------------------- random

BOOLEAN XBEM_GUEST_ABI hle_SystemFunction036(void* buffer, ULONG length) {  // RtlGenRandom
  if (buffer) fill_random(buffer, length);
  return 1;
}

// ---------------------------------------------------------------------- COM
// No COM servers exist, so object creation fails with "class not registered".

HRESULT XBEM_GUEST_ABI hle_CoInitializeEx(void*, DWORD) { return S_OK; }
HRESULT XBEM_GUEST_ABI hle_CoInitialize(void*) { return S_OK; }
void XBEM_GUEST_ABI hle_CoUninitialize() {}

HRESULT XBEM_GUEST_ABI hle_CoCreateInstance(const void* clsid, void*, DWORD, const void* iid, void** out) {
  if (out) *out = nullptr;
  XBEM_WARN("kernelx", "CoCreateInstance(clsid {}, iid {}) -> REGDB_E_CLASSNOTREG", guid_text(clsid), guid_text(iid));
  return REGDB_E_CLASSNOTREG;
}

HRESULT XBEM_GUEST_ABI hle_CoGetClassObject(const void* clsid, DWORD, void*, const void*, void** out) {
  if (out) *out = nullptr;
  XBEM_WARN("kernelx", "CoGetClassObject({}) -> REGDB_E_CLASSNOTREG", guid_text(clsid));
  return REGDB_E_CLASSNOTREG;
}

void* XBEM_GUEST_ABI hle_CoTaskMemAlloc(SIZE_T n) { return heap_alloc(n, false); }
void XBEM_GUEST_ABI hle_CoTaskMemFree(void* p) {
  if (p) heap_free(p);
}
void* XBEM_GUEST_ABI hle_CoTaskMemRealloc(void* p, SIZE_T n) { return heap_realloc(p, n, false); }

HRESULT XBEM_GUEST_ABI hle_CoCreateGuid(uint8_t* g) {
  if (!g) return E_INVALIDARG;
  fill_random(g, 16);
  g[7] = static_cast<uint8_t>((g[7] & 0x0F) | 0x40);  // version 4 (Data3 is little-endian)
  g[8] = static_cast<uint8_t>((g[8] & 0x3F) | 0x80);  // variant
  return S_OK;
}

INT XBEM_GUEST_ABI hle_StringFromGUID2(const uint8_t* g, char16_t* out, INT cch) {
  if (!g || !out || cch < 39) return 0;
  const std::string s = "{" + guid_to_string(g) + "}";
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 32);
    out[i] = static_cast<char16_t>(c);
  }
  out[s.size()] = 0;
  return static_cast<INT>(s.size() + 1);
}

bool parse_guid(const char16_t* text, uint8_t* g) {
  std::string s = narrow(text);
  if (s.size() == 38 && s.front() == '{' && s.back() == '}') s = s.substr(1, 36);
  if (s.size() != 36) return false;
  auto hex = [&](size_t pos, int digits, uint64_t& v) {
    v = 0;
    for (int i = 0; i < digits; ++i) {
      const char c = s[pos + static_cast<size_t>(i)];
      int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      v = v * 16 + static_cast<uint64_t>(d);
    }
    return true;
  };
  uint64_t a, b, c, d, e;
  if (!hex(0, 8, a) || !hex(9, 4, b) || !hex(14, 4, c) || !hex(19, 4, d) || !hex(24, 12, e)) return false;
  const auto a32 = static_cast<uint32_t>(a);
  const auto b16 = static_cast<uint16_t>(b), c16 = static_cast<uint16_t>(c);
  std::memcpy(g, &a32, 4);
  std::memcpy(g + 4, &b16, 2);
  std::memcpy(g + 6, &c16, 2);
  g[8] = static_cast<uint8_t>(d >> 8);
  g[9] = static_cast<uint8_t>(d);
  for (int i = 0; i < 6; ++i) g[10 + i] = static_cast<uint8_t>(e >> (40 - 8 * i));
  return true;
}

HRESULT XBEM_GUEST_ABI hle_CLSIDFromString(const char16_t* s, uint8_t* g) {
  return (s && g && parse_guid(s, g)) ? S_OK : static_cast<HRESULT>(0x800401F3);  // CO_E_CLASSSTRING
}
HRESULT XBEM_GUEST_ABI hle_IIDFromString(const char16_t* s, uint8_t* g) { return hle_CLSIDFromString(s, g); }

HRESULT XBEM_GUEST_ABI hle_CoGetApartmentType(int32_t* type, int32_t* qualifier) {
  if (type) *type = 1;  // APTTYPE_MTA
  if (qualifier) *qualifier = 0;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_CoIncrementMTAUsage(uint64_t* cookie) {
  if (cookie) *cookie = 1;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_CoDecrementMTAUsage(uint64_t) { return S_OK; }
HRESULT XBEM_GUEST_ABI hle_CoGetObjectContext(const void*, void** out) {
  if (out) *out = nullptr;
  return E_NOINTERFACE;
}
HRESULT XBEM_GUEST_ABI hle_CoCreateFreeThreadedMarshaler(void*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL;
}
HRESULT XBEM_GUEST_ABI hle_PropVariantClear(void* pv) {
  if (pv) std::memset(pv, 0, 24);
  return S_OK;
}

// ------------------------------------------------------------ WinRT strings
// HSTRING points at a header {flags, length, reserved, buffer}; owned strings
// keep their characters (NUL-terminated) right after it. Reference strings
// use the caller's 24-byte HSTRING_HEADER. NULL is the empty string.

struct HString {
  uint32_t flags;  // 1 = reference (not owned)
  uint32_t length;
  std::atomic<int64_t> refs;
  const char16_t* buffer;
};
static_assert(sizeof(HString) == 24);

const char16_t g_empty_hstring[1] = {0};

HString* make_hstring(const char16_t* src, uint32_t len) {
  auto* h = static_cast<HString*>(heap_alloc(sizeof(HString) + (len + 1) * 2, false));
  if (!h) return nullptr;
  h->flags = 0;
  h->length = len;
  new (&h->refs) std::atomic<int64_t>(1);
  auto* data = reinterpret_cast<char16_t*>(h + 1);
  if (len && src) std::memcpy(data, src, len * 2u);
  data[len] = 0;
  h->buffer = data;
  return h;
}

HRESULT XBEM_GUEST_ABI hle_WindowsCreateString(const char16_t* src, UINT len, HString** out) {
  if (!out) return E_INVALIDARG;
  *out = nullptr;
  if (len == 0) return S_OK;
  if (!src) return E_POINTER;
  *out = make_hstring(src, len);
  return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT XBEM_GUEST_ABI hle_WindowsCreateStringReference(const char16_t* src, UINT len, HString* header, HString** out) {
  if (!out || !header) return E_INVALIDARG;
  *out = nullptr;
  if (len == 0) return S_OK;
  if (!src || src[len] != 0) return E_INVALIDARG;
  header->flags = 1;
  header->length = len;
  new (&header->refs) std::atomic<int64_t>(0);
  header->buffer = src;
  *out = header;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_WindowsDeleteString(HString* s) {
  if (s && !(s->flags & 1) && --s->refs == 0) heap_free(s);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_WindowsDuplicateString(HString* s, HString** out) {
  if (!out) return E_INVALIDARG;
  if (!s) {
    *out = nullptr;
    return S_OK;
  }
  if (s->flags & 1) {  // references are copied into an owned string
    *out = make_hstring(s->buffer, s->length);
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  ++s->refs;
  *out = s;
  return S_OK;
}

UINT XBEM_GUEST_ABI hle_WindowsGetStringLen(HString* s) { return s ? s->length : 0; }

const char16_t* XBEM_GUEST_ABI hle_WindowsGetStringRawBuffer(HString* s, UINT* len) {
  if (len) *len = s ? s->length : 0;
  return s ? s->buffer : g_empty_hstring;
}

BOOL XBEM_GUEST_ABI hle_WindowsIsStringEmpty(HString* s) { return (!s || s->length == 0) ? TRUE : FALSE; }

HRESULT XBEM_GUEST_ABI hle_WindowsStringHasEmbeddedNull(HString* s, BOOL* out) {
  if (!out) return E_INVALIDARG;
  *out = FALSE;
  for (UINT i = 0; s && i < s->length; ++i)
    if (s->buffer[i] == 0) *out = TRUE;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_WindowsCompareStringOrdinal(HString* a, HString* b, INT* result) {
  if (!result) return E_INVALIDARG;
  const std::u16string_view x = a ? std::u16string_view(a->buffer, a->length) : std::u16string_view();
  const std::u16string_view y = b ? std::u16string_view(b->buffer, b->length) : std::u16string_view();
  const int c = x.compare(y);
  *result = c < 0 ? -1 : c > 0 ? 1 : 0;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_WindowsConcatString(HString* a, HString* b, HString** out) {
  if (!out) return E_INVALIDARG;
  std::u16string s;
  if (a) s.append(a->buffer, a->length);
  if (b) s.append(b->buffer, b->length);
  return hle_WindowsCreateString(s.data(), static_cast<UINT>(s.size()), out);
}

HRESULT XBEM_GUEST_ABI hle_WindowsSubstring(HString* s, UINT start, HString** out) {
  if (!out) return E_INVALIDARG;
  const UINT len = s ? s->length : 0;
  if (start > len) return static_cast<HRESULT>(0x8000000B);  // E_BOUNDS
  return hle_WindowsCreateString(s ? s->buffer + start : nullptr, len - start, out);
}

HRESULT XBEM_GUEST_ABI hle_WindowsSubstringWithSpecifiedLength(HString* s, UINT start, UINT n, HString** out) {
  if (!out) return E_INVALIDARG;
  const UINT len = s ? s->length : 0;
  if (start > len || n > len - start) return static_cast<HRESULT>(0x8000000B);
  return hle_WindowsCreateString(s ? s->buffer + start : nullptr, n, out);
}

// String buffers: an owned HString handed out before it is "promoted".
HRESULT XBEM_GUEST_ABI hle_WindowsPreallocateStringBuffer(UINT len, char16_t** chars, HString** buffer) {
  if (!chars || !buffer) return E_POINTER;
  HString* h = make_hstring(nullptr, len);
  if (!h) return E_OUTOFMEMORY;
  *chars = const_cast<char16_t*>(h->buffer);
  *buffer = h;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_WindowsPromoteStringBuffer(HString* buffer, HString** out) {
  if (!out) return E_POINTER;
  *out = (buffer && buffer->length) ? buffer : nullptr;
  if (buffer && !buffer->length) heap_free(buffer);
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_WindowsDeleteStringBuffer(HString* buffer) {
  if (buffer) heap_free(buffer);
  return S_OK;
}

// ----------------------------------------------------- WinRT activation/errors

HRESULT XBEM_GUEST_ABI hle_RoInitialize(int32_t) { return S_OK; }
void XBEM_GUEST_ABI hle_RoUninitialize() {}

std::string hstring_text(HString* s) { return s ? utf16_to_utf8(std::u16string_view(s->buffer, s->length)) : ""; }

HRESULT XBEM_GUEST_ABI hle_RoGetActivationFactory(HString* cls, const void* iid, void** out) {
  if (out) *out = nullptr;
  XBEM_WARN("kernelx", "RoGetActivationFactory({}, iid {}) -> CLASS_E_CLASSNOTAVAILABLE", hstring_text(cls),
            guid_text(iid));
  return CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT XBEM_GUEST_ABI hle_RoActivateInstance(HString* cls, void** out) {
  if (out) *out = nullptr;
  XBEM_WARN("kernelx", "RoActivateInstance({}) -> CLASS_E_CLASSNOTAVAILABLE", hstring_text(cls));
  return CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT XBEM_GUEST_ABI hle_RoGetApartmentIdentifier(uint64_t* id) {
  if (id) *id = 1;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_RoRegisterForApartmentShutdown(void*, uint64_t* id, void** cookie) {
  if (id) *id = 1;
  if (cookie) *cookie = nullptr;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_RoUnregisterForApartmentShutdown(void*) { return S_OK; }

BOOL XBEM_GUEST_ABI hle_RoOriginateError(HRESULT hr, HString* msg) {
  XBEM_DEBUG("kernelx", "RoOriginateError({:#x}, \"{}\")", static_cast<uint32_t>(hr), hstring_text(msg));
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_RoOriginateErrorW(HRESULT hr, UINT len, const char16_t* msg) {
  XBEM_DEBUG("kernelx", "RoOriginateErrorW({:#x}, \"{}\")", static_cast<uint32_t>(hr),
             msg ? utf16_to_utf8(std::u16string_view(msg, len ? len : bounded_strlen16(msg))) : "");
  return TRUE;
}
BOOL XBEM_GUEST_ABI hle_RoOriginateLanguageException(HRESULT hr, HString* msg, void*) { return hle_RoOriginateError(hr, msg); }
BOOL XBEM_GUEST_ABI hle_RoTransformError(HRESULT, HRESULT, HString*) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_RoTransformErrorW(HRESULT, HRESULT, UINT, const char16_t*) { return TRUE; }
HRESULT XBEM_GUEST_ABI hle_RoCaptureErrorContext(HRESULT) { return S_OK; }
void XBEM_GUEST_ABI hle_RoClearError() {}
HRESULT XBEM_GUEST_ABI hle_RoGetErrorReportingFlags(uint32_t* flags) {
  if (flags) *flags = 0;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_RoSetErrorReportingFlags(uint32_t) { return S_OK; }
HRESULT XBEM_GUEST_ABI hle_GetRestrictedErrorInfo(void** out) {
  if (out) *out = nullptr;
  return S_FALSE;
}
HRESULT XBEM_GUEST_ABI hle_SetRestrictedErrorInfo(void*) { return S_OK; }
[[noreturn]] void XBEM_GUEST_ABI hle_RoFailFastWithErrorContext(HRESULT hr) {
  XBEM_ERROR("kernelx", "RoFailFastWithErrorContext({:#x})", static_cast<uint32_t>(hr));
  proc().exit_process(static_cast<uint32_t>(hr));
}

// ------------------------------------------------------- oleaut32 (BSTR)
// A BSTR points at its characters; the byte length sits in the 4 bytes before.

char16_t* alloc_bstr(const char16_t* src, uint32_t chars) {
  auto* block = static_cast<uint8_t*>(heap_alloc(4 + (chars + 1) * 2u, false));
  if (!block) return nullptr;
  const uint32_t bytes = chars * 2;
  std::memcpy(block, &bytes, 4);
  auto* s = reinterpret_cast<char16_t*>(block + 4);
  if (src) std::memcpy(s, src, bytes);
  else std::memset(s, 0, bytes);
  s[chars] = 0;
  return s;
}

char16_t* XBEM_GUEST_ABI hle_SysAllocString(const char16_t* s) {
  return s ? alloc_bstr(s, static_cast<uint32_t>(bounded_strlen16(s))) : nullptr;
}
char16_t* XBEM_GUEST_ABI hle_SysAllocStringLen(const char16_t* s, UINT n) { return alloc_bstr(s, n); }
void XBEM_GUEST_ABI hle_SysFreeString(char16_t* s) {
  if (s) heap_free(reinterpret_cast<uint8_t*>(s) - 4);
}
UINT XBEM_GUEST_ABI hle_SysStringLen(const char16_t* s) {
  if (!s) return 0;
  uint32_t bytes;
  std::memcpy(&bytes, reinterpret_cast<const uint8_t*>(s) - 4, 4);
  return bytes / 2;
}
UINT XBEM_GUEST_ABI hle_SysStringByteLen(const char16_t* s) { return hle_SysStringLen(s) * 2; }
void XBEM_GUEST_ABI hle_VariantInit(void* v) {
  if (v) std::memset(v, 0, 24);
}
HRESULT XBEM_GUEST_ABI hle_VariantClear(void* v) {
  if (v) std::memset(v, 0, 24);  // VT_EMPTY; owned BSTR/interfaces are not released
  return S_OK;
}

// ------------------------------------------------------------ bcrypt (RNG)

LONG XBEM_GUEST_ABI hle_BCryptOpenAlgorithmProvider(uint64_t* handle, LPCWSTR algorithm, LPCWSTR, ULONG) {
  if (!handle) return static_cast<LONG>(0xC000000D);  // STATUS_INVALID_PARAMETER
  const std::string a = narrow(algorithm);
  if (a != "RNG" && a != "FIPS186DSARNG" && a != "DUALECRNG") {
    XBEM_WARN("bcrypt", "BCryptOpenAlgorithmProvider({}) -> STATUS_NOT_FOUND (only RNG is available)", a);
    *handle = 0;
    return static_cast<LONG>(0xC0000225);  // STATUS_NOT_FOUND
  }
  *handle = 0x52'4E47;  // "RNG"
  return 0;
}
LONG XBEM_GUEST_ABI hle_BCryptCloseAlgorithmProvider(uint64_t, ULONG) { return 0; }
LONG XBEM_GUEST_ABI hle_BCryptGenRandom(uint64_t, void* buffer, ULONG length, ULONG) {
  if (!buffer && length) return static_cast<LONG>(0xC000000D);
  fill_random(buffer, length);
  return 0;
}

}  // namespace

void add_kernelx_system_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(RegOpenKeyW), XBEM_HLE(RegOpenKeyA), XBEM_HLE(RegOpenKeyExW), XBEM_HLE(RegOpenKeyExA),
      XBEM_HLE(RegCreateKeyExW), XBEM_HLE(RegQueryValueExW), XBEM_HLE(RegQueryValueExA), XBEM_HLE(RegGetValueW),
      XBEM_HLE(RegSetValueExW), XBEM_HLE(RegDeleteValueW), XBEM_HLE(RegEnumKeyExW), XBEM_HLE(RegEnumValueW),
      XBEM_HLE(RegQueryInfoKeyW), XBEM_HLE(RegCloseKey), XBEM_HLE(RegNotifyChangeKeyValue), XBEM_HLE(EventRegister),
      XBEM_HLE(EventUnregister), XBEM_HLE(EventSetInformation), XBEM_HLE(EventWrite), XBEM_HLE(EventWriteTransfer),
      XBEM_HLE(EventWriteEx), XBEM_HLE(EventWriteString), XBEM_HLE(EventEnabled), XBEM_HLE(EventProviderEnabled),
      XBEM_HLE(EventActivityIdControl), XBEM_HLE(RegisterTraceGuidsW), XBEM_HLE(UnregisterTraceGuids),
      XBEM_HLE(GetTraceLoggerHandle), XBEM_HLE(GetTraceEnableLevel), XBEM_HLE(GetTraceEnableFlags),
      XBEM_HLE(TraceEvent), XBEM_HLE(TraceMessage), XBEM_HLE(SystemFunction036), XBEM_HLE(CoInitializeEx),
      XBEM_HLE(CoInitialize), XBEM_HLE(CoUninitialize), XBEM_HLE(CoCreateInstance), XBEM_HLE(CoGetClassObject),
      XBEM_HLE(CoTaskMemAlloc), XBEM_HLE(CoTaskMemFree), XBEM_HLE(CoTaskMemRealloc), XBEM_HLE(CoCreateGuid),
      XBEM_HLE(StringFromGUID2), XBEM_HLE(CLSIDFromString), XBEM_HLE(IIDFromString), XBEM_HLE(CoGetApartmentType),
      XBEM_HLE(CoIncrementMTAUsage), XBEM_HLE(CoDecrementMTAUsage), XBEM_HLE(CoGetObjectContext),
      XBEM_HLE(CoCreateFreeThreadedMarshaler), XBEM_HLE(PropVariantClear), XBEM_HLE(WindowsCreateString),
      XBEM_HLE(WindowsCreateStringReference), XBEM_HLE(WindowsDeleteString), XBEM_HLE(WindowsDuplicateString),
      XBEM_HLE(WindowsGetStringLen), XBEM_HLE(WindowsGetStringRawBuffer), XBEM_HLE(WindowsIsStringEmpty),
      XBEM_HLE(WindowsStringHasEmbeddedNull), XBEM_HLE(WindowsCompareStringOrdinal), XBEM_HLE(WindowsConcatString),
      XBEM_HLE(WindowsSubstring), XBEM_HLE(WindowsSubstringWithSpecifiedLength),
      XBEM_HLE(WindowsPreallocateStringBuffer), XBEM_HLE(WindowsPromoteStringBuffer),
      XBEM_HLE(WindowsDeleteStringBuffer), XBEM_HLE(RoInitialize), XBEM_HLE(RoUninitialize),
      XBEM_HLE(RoGetActivationFactory), XBEM_HLE(RoActivateInstance), XBEM_HLE(RoGetApartmentIdentifier),
      XBEM_HLE(RoRegisterForApartmentShutdown), XBEM_HLE(RoUnregisterForApartmentShutdown),
      XBEM_HLE(RoOriginateError), XBEM_HLE(RoOriginateErrorW), XBEM_HLE(RoOriginateLanguageException),
      XBEM_HLE(RoTransformError), XBEM_HLE(RoTransformErrorW), XBEM_HLE(RoCaptureErrorContext), XBEM_HLE(RoClearError),
      XBEM_HLE(RoGetErrorReportingFlags), XBEM_HLE(RoSetErrorReportingFlags), XBEM_HLE(GetRestrictedErrorInfo),
      XBEM_HLE(SetRestrictedErrorInfo), XBEM_HLE(RoFailFastWithErrorContext),
  });
}

ModuleDef make_oleaut32_module() {
  ModuleDef m;
  m.name = "oleaut32.dll";
  m.description = "OLE automation: BSTR and VARIANT basics";
  m.exports = {
      XBEM_HLE(SysAllocString), XBEM_HLE(SysAllocStringLen), XBEM_HLE(SysFreeString), XBEM_HLE(SysStringLen),
      XBEM_HLE(SysStringByteLen), XBEM_HLE(VariantInit), XBEM_HLE(VariantClear),
  };
  return m;
}

ModuleDef make_bcrypt_module() {
  ModuleDef m;
  m.name = "bcrypt.dll";
  m.description = "CNG: random numbers only (BCryptGenRandom)";
  m.exports = {XBEM_HLE(BCryptOpenAlgorithmProvider), XBEM_HLE(BCryptCloseAlgorithmProvider), XBEM_HLE(BCryptGenRandom)};
  return m;
}

}  // namespace xbem::hle
