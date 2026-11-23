// Networking for titles: Winsock (ws2_32), WinHTTP and the certificate store
// (crypt32).
//
// The emulator runs titles offline: Winsock starts up, but every socket
// operation reports a dead network and name resolution finds no hosts;
// WinHTTP sessions and requests can be created and configured, but sending a
// request fails as if the server could not be reached; certificate stores
// are empty. Titles handle all of these as "no connection", which is how a
// console without network behaves - and nothing leaves the host.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;

// ---------------------------------------------------------------- ws2_32

using SOCKET = uint64_t;
constexpr SOCKET INVALID_SOCKET = ~0ull;
constexpr int SOCKET_ERROR = -1;
constexpr DWORD WSAENOTSOCK = 10038;
constexpr DWORD WSAENETDOWN = 10050;
constexpr DWORD WSANOTINITIALISED = 10093;
constexpr DWORD WSAHOST_NOT_FOUND = 11001;
constexpr int EAI_NONAME = 11001;

std::atomic<int> g_wsa_started{0};

void log_offline_once() {
  XBEM_WARN_ONCE("net", "the title uses sockets - xbem runs titles offline, all network operations fail");
}

int socket_error(DWORD error = WSAENOTSOCK) {
  set_last_error(g_wsa_started.load() ? error : WSANOTINITIALISED);
  return SOCKET_ERROR;
}

// WSADATA (x64): WORD wVersion, wHighVersion; unsigned short iMaxSockets, iMaxUdpDg;
// char* lpVendorInfo; char szDescription[257]; char szSystemStatus[129];
int32_t XBEM_GUEST_ABI hle_WSAStartup(WORD version, uint8_t* data) {
  if (!data) return 10014;  // WSAEFAULT
  std::memset(data, 0, 408);
  const WORD v = std::min<WORD>(version, 0x0202), high = 0x0202, max_sockets = 0, max_udp = 0;
  std::memcpy(data + 0, &v, 2);
  std::memcpy(data + 2, &high, 2);
  std::memcpy(data + 4, &max_sockets, 2);
  std::memcpy(data + 6, &max_udp, 2);
  std::snprintf(reinterpret_cast<char*>(data + 16), 257, "xbem offline Winsock 2.2");
  std::snprintf(reinterpret_cast<char*>(data + 16 + 257), 129, "Running (offline)");
  g_wsa_started.fetch_add(1);
  return 0;
}

int32_t XBEM_GUEST_ABI hle_WSACleanup() {
  if (g_wsa_started.load() <= 0) return socket_error(WSANOTINITIALISED);
  g_wsa_started.fetch_sub(1);
  return 0;
}

int32_t XBEM_GUEST_ABI hle_WSAGetLastError() { return static_cast<int32_t>(last_error()); }
void XBEM_GUEST_ABI hle_WSASetLastError(int32_t e) { set_last_error(static_cast<DWORD>(e)); }

SOCKET XBEM_GUEST_ABI hle_socket(int32_t af, int32_t type, int32_t protocol) {
  log_offline_once();
  XBEM_DEBUG("net", "socket({}, {}, {}) -> WSAENETDOWN", af, type, protocol);
  socket_error(WSAENETDOWN);
  return INVALID_SOCKET;
}
SOCKET XBEM_GUEST_ABI hle_WSASocketW(int32_t af, int32_t type, int32_t protocol, void*, uint32_t, DWORD) {
  return hle_socket(af, type, protocol);
}
SOCKET XBEM_GUEST_ABI hle_accept(SOCKET, void*, int32_t*) {
  socket_error();
  return INVALID_SOCKET;
}

// Every operation on a socket fails: no socket can exist.
int32_t XBEM_GUEST_ABI hle_bind(SOCKET, const void*, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_closesocket(SOCKET) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_connect(SOCKET, const void*, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_getpeername(SOCKET, void*, int32_t*) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_getsockname(SOCKET, void*, int32_t*) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_getsockopt(SOCKET, int32_t, int32_t, char*, int32_t*) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_setsockopt(SOCKET, int32_t, int32_t, const char*, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_ioctlsocket(SOCKET, int32_t, uint32_t*) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_listen(SOCKET, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_recv(SOCKET, char*, int32_t, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_recvfrom(SOCKET, char*, int32_t, int32_t, void*, int32_t*) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_send(SOCKET, const char*, int32_t, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_sendto(SOCKET, const char*, int32_t, int32_t, const void*, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_shutdown(SOCKET, int32_t) { return socket_error(); }
int32_t XBEM_GUEST_ABI hle_WSAIoctl(SOCKET, DWORD, void*, DWORD, void*, DWORD, DWORD*, void*, void*) {
  return socket_error();
}

// fd_set (x64): u_int fd_count; SOCKET fd_array[64];
int32_t XBEM_GUEST_ABI hle_select(int32_t, uint8_t* r, uint8_t* w, uint8_t* e, const void* timeout) {
  const auto count = [](const uint8_t* set) {
    uint32_t n = 0;
    if (set) std::memcpy(&n, set, 4);
    return n;
  };
  if (count(r) + count(w) + count(e) > 0) return socket_error();
  (void)timeout;
  return socket_error(10022);  // WSAEINVAL: no sockets at all (as Windows reports)
}

int32_t XBEM_GUEST_ABI hle___WSAFDIsSet(SOCKET s, const uint8_t* set) {
  if (!set) return 0;
  uint32_t n;
  std::memcpy(&n, set, 4);
  for (uint32_t i = 0; i < std::min<uint32_t>(n, 64); ++i) {
    SOCKET v;
    std::memcpy(&v, set + 8 + i * 8, 8);
    if (v == s) return 1;
  }
  return 0;
}

uint32_t XBEM_GUEST_ABI hle_htonl(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
uint16_t XBEM_GUEST_ABI hle_htons(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }
uint32_t XBEM_GUEST_ABI hle_ntohl(uint32_t v) { return hle_htonl(v); }
uint16_t XBEM_GUEST_ABI hle_ntohs(uint16_t v) { return hle_htons(v); }

// inet_addr: dotted quad -> network-order address, INADDR_NONE on error.
uint32_t XBEM_GUEST_ABI hle_inet_addr(const char* text) {
  if (!text) return 0xFFFFFFFF;
  unsigned a, b, c, d;
  char tail;
  if (std::sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return 0xFFFFFFFF;
  return a | (b << 8) | (c << 16) | (d << 24);
}

char* XBEM_GUEST_ABI hle_inet_ntoa(uint32_t addr) {
  thread_local char buf[16];
  std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, addr >> 24);
  return buf;
}

int32_t XBEM_GUEST_ABI hle_gethostname(char* name, int32_t len) {
  constexpr char kName[] = "xbox";
  if (!name || len < static_cast<int32_t>(sizeof kName)) return socket_error(10014);
  std::memcpy(name, kName, sizeof kName);
  return 0;
}

void* XBEM_GUEST_ABI hle_gethostbyname(const char* name) {
  log_offline_once();
  XBEM_DEBUG("net", "gethostbyname({}) -> not found (offline)", narrow(name));
  set_last_error(WSAHOST_NOT_FOUND);
  return nullptr;
}
void* XBEM_GUEST_ABI hle_gethostbyaddr(const char*, int32_t, int32_t) {
  set_last_error(WSAHOST_NOT_FOUND);
  return nullptr;
}

int32_t XBEM_GUEST_ABI hle_getaddrinfo(const char* node, const char* service, const void*, void** result) {
  if (result) *result = nullptr;
  log_offline_once();
  XBEM_DEBUG("net", "getaddrinfo({}, {}) -> EAI_NONAME (offline)", node ? narrow(node) : "(null)",
             service ? narrow(service) : "(null)");
  set_last_error(WSAHOST_NOT_FOUND);
  return EAI_NONAME;
}
int32_t XBEM_GUEST_ABI hle_GetAddrInfoW(const char16_t* node, const char16_t*, const void*, void** result) {
  if (result) *result = nullptr;
  log_offline_once();
  XBEM_DEBUG("net", "GetAddrInfoW({}) -> EAI_NONAME (offline)", node ? narrow(node) : "(null)");
  set_last_error(WSAHOST_NOT_FOUND);
  return EAI_NONAME;
}
void XBEM_GUEST_ABI hle_freeaddrinfo(void*) {}
void XBEM_GUEST_ABI hle_FreeAddrInfoW(void*) {}

// --------------------------------------------------------------- winhttp

constexpr uint64_t kHttpMagic = 0x5054544842454258ull;  // "XBEBHTTP"
constexpr DWORD ERROR_WINHTTP_INVALID_URL = 12005;
constexpr DWORD ERROR_WINHTTP_INCORRECT_HANDLE_STATE = 12019;
constexpr DWORD ERROR_WINHTTP_CANNOT_CONNECT = 12029;
constexpr DWORD WINHTTP_OPTION_CONTEXT_VALUE = 45;

struct HttpHandle {
  uint64_t magic = kHttpMagic;
  int kind = 0;  // 0 session, 1 connection, 2 request
  HttpHandle* parent = nullptr;
  uint64_t callback = 0;
  uint64_t context = 0;
  std::string host;
};

HttpHandle* http_handle(void* h) {
  auto* p = static_cast<HttpHandle*>(h);
  return (p && p->magic == kHttpMagic) ? p : nullptr;
}

void* new_http_handle(int kind, HttpHandle* parent) {
  auto* h = new HttpHandle;
  h->kind = kind;
  h->parent = parent;
  if (parent) {
    h->callback = parent->callback;
    h->host = parent->host;
  }
  return h;
}

void* XBEM_GUEST_ABI hle_WinHttpOpen(const char16_t* agent, DWORD, const char16_t*, const char16_t*, DWORD flags) {
  XBEM_INFO("net", "WinHttpOpen(agent \"{}\", flags {:#x}): offline session (requests will fail to connect)",
            agent ? narrow(agent) : "", flags);
  return new_http_handle(0, nullptr);
}

void* XBEM_GUEST_ABI hle_WinHttpConnect(void* session, const char16_t* server, uint16_t port, DWORD) {
  HttpHandle* s = http_handle(session);
  if (!s || !server) {
    set_last_error(ERROR_INVALID_HANDLE);
    return nullptr;
  }
  auto* c = static_cast<HttpHandle*>(new_http_handle(1, s));
  c->host = narrow(server) + ":" + std::to_string(port);
  return c;
}

void* XBEM_GUEST_ABI hle_WinHttpOpenRequest(void* connection, const char16_t* verb, const char16_t* object,
                                            const char16_t*, const char16_t*, const char16_t* const*, DWORD) {
  HttpHandle* c = http_handle(connection);
  if (!c) {
    set_last_error(ERROR_INVALID_HANDLE);
    return nullptr;
  }
  auto* r = static_cast<HttpHandle*>(new_http_handle(2, c));
  XBEM_DEBUG("net", "WinHttpOpenRequest {} {}{}", verb ? narrow(verb) : "GET", c->host, object ? narrow(object) : "/");
  return r;
}

BOOL XBEM_GUEST_ABI hle_WinHttpCloseHandle(void* h) {
  HttpHandle* p = http_handle(h);
  if (!p) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  p->magic = 0;
  delete p;
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_WinHttpSetOption(void* h, DWORD option, const void* buffer, DWORD length) {
  HttpHandle* p = http_handle(h);
  if (p && option == WINHTTP_OPTION_CONTEXT_VALUE && buffer && length >= 8) std::memcpy(&p->context, buffer, 8);
  return TRUE;  // options are accepted (no effect offline)
}

BOOL XBEM_GUEST_ABI hle_WinHttpQueryOption(void* h, DWORD option, void* buffer, DWORD* length) {
  HttpHandle* p = http_handle(h);
  if (p && option == WINHTTP_OPTION_CONTEXT_VALUE && length) {
    if (buffer && *length >= 8) {
      std::memcpy(buffer, &p->context, 8);
      *length = 8;
      return TRUE;
    }
    *length = 8;
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }
  set_last_error(ERROR_INVALID_PARAMETER);
  return FALSE;
}

BOOL XBEM_GUEST_ABI hle_WinHttpSetTimeouts(void*, int32_t, int32_t, int32_t, int32_t) { return TRUE; }
BOOL XBEM_GUEST_ABI hle_WinHttpAddRequestHeaders(void*, const char16_t*, DWORD, DWORD) { return TRUE; }

uint64_t XBEM_GUEST_ABI hle_WinHttpSetStatusCallback(void* h, uint64_t callback, DWORD, uint64_t) {
  HttpHandle* p = http_handle(h);
  if (!p) {
    set_last_error(ERROR_INVALID_HANDLE);
    return ~0ull;  // WINHTTP_INVALID_STATUS_CALLBACK
  }
  return std::exchange(p->callback, callback);
}

BOOL XBEM_GUEST_ABI hle_WinHttpSendRequest(void* h, const char16_t*, DWORD, void*, DWORD, DWORD, uint64_t context) {
  HttpHandle* p = http_handle(h);
  if (p) p->context = context;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 5)
    XBEM_INFO("net", "WinHttpSendRequest to {} -> ERROR_WINHTTP_CANNOT_CONNECT (offline)", p ? p->host : "?");
  set_last_error(ERROR_WINHTTP_CANNOT_CONNECT);
  return FALSE;
}

BOOL XBEM_GUEST_ABI hle_WinHttpReceiveResponse(void*, void*) {
  set_last_error(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
  return FALSE;
}
BOOL XBEM_GUEST_ABI hle_WinHttpQueryDataAvailable(void*, DWORD* available) {
  if (available) *available = 0;
  set_last_error(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
  return FALSE;
}
BOOL XBEM_GUEST_ABI hle_WinHttpReadData(void*, void*, DWORD, DWORD* read) {
  if (read) *read = 0;
  set_last_error(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
  return FALSE;
}
BOOL XBEM_GUEST_ABI hle_WinHttpWriteData(void*, const void*, DWORD, DWORD* written) {
  if (written) *written = 0;
  set_last_error(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
  return FALSE;
}
BOOL XBEM_GUEST_ABI hle_WinHttpQueryHeaders(void*, DWORD, const char16_t*, void*, DWORD*, DWORD*) {
  set_last_error(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
  return FALSE;
}

// URL_COMPONENTS (x64): DWORD dwStructSize; LPWSTR lpszScheme; DWORD dwSchemeLength; INTERNET_SCHEME nScheme;
// LPWSTR lpszHostName; DWORD dwHostNameLength; INTERNET_PORT nPort; LPWSTR lpszUserName; DWORD dwUserNameLength;
// LPWSTR lpszPassword; DWORD dwPasswordLength; LPWSTR lpszUrlPath; DWORD dwUrlPathLength; LPWSTR lpszExtraInfo;
// DWORD dwExtraInfoLength;
struct UrlComponents {
  DWORD size;
  uint32_t pad0;
  char16_t* scheme;
  DWORD scheme_length;
  int32_t scheme_id;
  char16_t* host;
  DWORD host_length;
  uint16_t port;
  uint16_t pad1;
  char16_t* user;
  DWORD user_length;
  uint32_t pad2;
  char16_t* password;
  DWORD password_length;
  uint32_t pad3;
  char16_t* path;
  DWORD path_length;
  uint32_t pad4;
  char16_t* extra;
  DWORD extra_length;
  uint32_t pad5;
};
static_assert(sizeof(UrlComponents) == 104);

// Fills one component: either points into the URL (buffer null, length nonzero)
// or copies into the caller's buffer.
bool set_component(const char16_t* url, std::u16string_view part, char16_t*& buffer, DWORD& length) {
  if (!buffer) {
    if (length) {
      buffer = const_cast<char16_t*>(part.empty() ? url : part.data());
      length = static_cast<DWORD>(part.size());
    }
    return true;
  }
  if (length <= part.size()) {
    length = static_cast<DWORD>(part.size() + 1);
    return false;
  }
  std::copy(part.begin(), part.end(), buffer);
  buffer[part.size()] = 0;
  length = static_cast<DWORD>(part.size());
  return true;
}

BOOL XBEM_GUEST_ABI hle_WinHttpCrackUrl(const char16_t* url, DWORD url_length, DWORD, UrlComponents* uc) {
  if (!url || !uc) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const std::u16string_view u = url_length ? std::u16string_view(url, url_length) : guest_wstr(url);
  const size_t colon = u.find(u"://");
  if (colon == std::u16string_view::npos) {
    set_last_error(ERROR_WINHTTP_INVALID_URL);
    return FALSE;
  }
  const std::u16string_view scheme = u.substr(0, colon);
  std::u16string_view rest = u.substr(colon + 3);
  const size_t path_start = rest.find_first_of(u"/?#");
  std::u16string_view authority = rest.substr(0, path_start);
  std::u16string_view path_all = path_start == std::u16string_view::npos ? u"" : rest.substr(path_start);
  std::u16string_view user, password;
  if (const size_t at = authority.rfind(u'@'); at != std::u16string_view::npos) {
    const std::u16string_view cred = authority.substr(0, at);
    authority = authority.substr(at + 1);
    const size_t c = cred.find(u':');
    user = cred.substr(0, c);
    if (c != std::u16string_view::npos) password = cred.substr(c + 1);
  }
  const bool https = scheme.size() == 5 && (scheme[4] == u's' || scheme[4] == u'S');
  uint16_t port = https ? 443 : 80;
  std::u16string_view host = authority;
  const size_t bracket = authority.rfind(u']');  // IPv6 literal "[::1]:8080"
  if (const size_t c = authority.rfind(u':');
      c != std::u16string_view::npos && (bracket == std::u16string_view::npos || c > bracket)) {
    host = authority.substr(0, c);
    port = 0;
    for (char16_t ch : authority.substr(c + 1)) port = static_cast<uint16_t>(port * 10 + (ch - u'0'));
  }
  const size_t extra_start = path_all.find_first_of(u"?#");
  const std::u16string_view path = path_all.substr(0, extra_start);
  const std::u16string_view extra = extra_start == std::u16string_view::npos ? u"" : path_all.substr(extra_start);

  uc->scheme_id = https ? 2 : 1;  // INTERNET_SCHEME_HTTPS / HTTP
  uc->port = port;
  bool ok = set_component(url, scheme, uc->scheme, uc->scheme_length);
  ok &= set_component(url, host, uc->host, uc->host_length);
  ok &= set_component(url, user, uc->user, uc->user_length);
  ok &= set_component(url, password, uc->password, uc->password_length);
  ok &= set_component(url, path, uc->path, uc->path_length);
  ok &= set_component(url, extra, uc->extra, uc->extra_length);
  if (!ok) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_WinHttpGetIEProxyConfigForCurrentUser(uint8_t* config) {
  if (config) std::memset(config, 0, 32);
  set_last_error(12180);  // ERROR_WINHTTP_AUTODETECTION_FAILED
  return FALSE;
}

// ---------------------------------------------------------------- crypt32

constexpr uint64_t kStoreMagic = 0x45524F5453424558ull;  // "XBESTORE"
constexpr HRESULT CRYPT_E_NOT_FOUND = static_cast<HRESULT>(0x80092004);

void* new_store() { return new uint64_t{kStoreMagic}; }

void* XBEM_GUEST_ABI hle_CertOpenStore(const char*, DWORD, uint64_t, DWORD, const void*) { return new_store(); }
void* XBEM_GUEST_ABI hle_CertOpenSystemStoreW(uint64_t, const char16_t* name) {
  XBEM_DEBUG("net", "CertOpenSystemStoreW({}): empty store", name ? narrow(name) : "");
  return new_store();
}
BOOL XBEM_GUEST_ABI hle_CertCloseStore(void* store, DWORD) {
  auto* s = static_cast<uint64_t*>(store);
  if (s && *s == kStoreMagic) {
    *s = 0;
    delete s;
  }
  return TRUE;
}
void* XBEM_GUEST_ABI hle_CertFindCertificateInStore(void*, DWORD, DWORD, DWORD, const void*, const void*) {
  set_last_error(static_cast<DWORD>(CRYPT_E_NOT_FOUND));
  return nullptr;
}
void* XBEM_GUEST_ABI hle_CertFindChainInStore(void*, DWORD, DWORD, DWORD, const void*, const void*) {
  set_last_error(static_cast<DWORD>(CRYPT_E_NOT_FOUND));
  return nullptr;
}
void XBEM_GUEST_ABI hle_CertFreeCertificateChain(const void*) {}
BOOL XBEM_GUEST_ABI hle_CertFreeCertificateContext(const void*) { return TRUE; }

// CryptStringToBinaryW: Base64 decoding (CRYPT_STRING_BASE64HEADER/BASE64/ANY).
int base64_value(char16_t c) {
  if (c >= u'A' && c <= u'Z') return c - u'A';
  if (c >= u'a' && c <= u'z') return c - u'a' + 26;
  if (c >= u'0' && c <= u'9') return c - u'0' + 52;
  if (c == u'+') return 62;
  if (c == u'/') return 63;
  return -1;
}

BOOL XBEM_GUEST_ABI hle_CryptStringToBinaryW(const char16_t* text, DWORD length, DWORD flags, uint8_t* out,
                                             DWORD* out_size, DWORD* skip, DWORD* used_flags) {
  if (!text || !out_size) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  std::u16string_view s = length ? std::u16string_view(text, length) : guest_wstr(text);
  const DWORD format = flags & 0xFFFF;
  if (format != 0 && format != 1 && format != 6 && format != 7) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  // Strip a PEM header/footer.
  if (const size_t b = s.find(u"-----BEGIN"); b != std::u16string_view::npos) {
    const size_t line_end = s.find(u'\n', b);
    const size_t e = s.find(u"-----END", line_end == std::u16string_view::npos ? b : line_end);
    s = s.substr(line_end == std::u16string_view::npos ? s.size() : line_end + 1,
                 e == std::u16string_view::npos ? std::u16string_view::npos : e - line_end - 1);
  }
  std::vector<uint8_t> bytes;
  uint32_t acc = 0;
  int bits = 0;
  for (char16_t c : s) {
    const int v = base64_value(c);
    if (v < 0) continue;  // whitespace, padding
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<uint8_t>(acc >> bits));
    }
  }
  if (skip) *skip = 0;
  if (used_flags) *used_flags = format == 0 ? 0 : 1;
  if (!out) {
    *out_size = static_cast<DWORD>(bytes.size());
    return TRUE;
  }
  if (*out_size < bytes.size()) {
    *out_size = static_cast<DWORD>(bytes.size());
    set_last_error(234);  // ERROR_MORE_DATA
    return FALSE;
  }
  std::memcpy(out, bytes.data(), bytes.size());
  *out_size = static_cast<DWORD>(bytes.size());
  return TRUE;
}

}  // namespace

ModuleDef make_ws2_32_module() {
  ModuleDef m;
  m.name = "ws2_32.dll";
  m.description = "Winsock 2 (offline: sockets report a dead network, no host resolves)";
  m.exports = {
      XBEM_HLE_STUB_ORD(accept, 1),       XBEM_HLE_STUB_ORD(bind, 2),         XBEM_HLE_STUB_ORD(closesocket, 3),
      XBEM_HLE_STUB_ORD(connect, 4),      XBEM_HLE_STUB_ORD(getpeername, 5),  XBEM_HLE_STUB_ORD(getsockname, 6),
      XBEM_HLE_STUB_ORD(getsockopt, 7),   XBEM_HLE_ORD(htonl, 8),             XBEM_HLE_ORD(htons, 9),
      XBEM_HLE_STUB_ORD(ioctlsocket, 10), XBEM_HLE_ORD(inet_addr, 11),        XBEM_HLE_ORD(inet_ntoa, 12),
      XBEM_HLE_STUB_ORD(listen, 13),      XBEM_HLE_ORD(ntohl, 14),            XBEM_HLE_ORD(ntohs, 15),
      XBEM_HLE_STUB_ORD(recv, 16),        XBEM_HLE_STUB_ORD(recvfrom, 17),    XBEM_HLE_STUB_ORD(select, 18),
      XBEM_HLE_STUB_ORD(send, 19),        XBEM_HLE_STUB_ORD(sendto, 20),      XBEM_HLE_STUB_ORD(setsockopt, 21),
      XBEM_HLE_STUB_ORD(shutdown, 22),    XBEM_HLE_STUB_ORD(socket, 23),      XBEM_HLE_STUB_ORD(gethostbyaddr, 51),
      XBEM_HLE_STUB_ORD(gethostbyname, 52), XBEM_HLE_ORD(gethostname, 57),    XBEM_HLE_ORD(WSAGetLastError, 111),
      XBEM_HLE_ORD(WSASetLastError, 112), XBEM_HLE_ORD(WSAStartup, 115),      XBEM_HLE_ORD(WSACleanup, 116),
      XBEM_HLE_ORD(__WSAFDIsSet, 151),    XBEM_HLE_STUB(getaddrinfo),         XBEM_HLE(freeaddrinfo),
      XBEM_HLE_STUB(GetAddrInfoW),        XBEM_HLE(FreeAddrInfoW),            XBEM_HLE_STUB(WSASocketW),
      XBEM_HLE_STUB(WSAIoctl),
  };
  return m;
}

ModuleDef make_winhttp_module() {
  ModuleDef m;
  m.name = "winhttp.dll";
  m.description = "WinHTTP (offline: handles work, requests fail with ERROR_WINHTTP_CANNOT_CONNECT)";
  m.exports = {
      XBEM_HLE(WinHttpOpen),           XBEM_HLE(WinHttpConnect),          XBEM_HLE(WinHttpOpenRequest),
      XBEM_HLE(WinHttpCloseHandle),    XBEM_HLE(WinHttpSetOption),        XBEM_HLE(WinHttpQueryOption),
      XBEM_HLE(WinHttpSetTimeouts),    XBEM_HLE(WinHttpAddRequestHeaders), XBEM_HLE(WinHttpSetStatusCallback),
      XBEM_HLE_STUB(WinHttpSendRequest), XBEM_HLE_STUB(WinHttpReceiveResponse),
      XBEM_HLE_STUB(WinHttpQueryDataAvailable), XBEM_HLE_STUB(WinHttpReadData), XBEM_HLE_STUB(WinHttpWriteData),
      XBEM_HLE_STUB(WinHttpQueryHeaders), XBEM_HLE(WinHttpCrackUrl), XBEM_HLE_STUB(WinHttpGetIEProxyConfigForCurrentUser),
  };
  return m;
}

ModuleDef make_crypt32_module() {
  ModuleDef m;
  m.name = "crypt32.dll";
  m.description = "Certificate stores (always empty) and Base64 decoding";
  m.exports = {
      XBEM_HLE_STUB(CertOpenStore), XBEM_HLE_STUB(CertOpenSystemStoreW), XBEM_HLE(CertCloseStore),
      XBEM_HLE_STUB(CertFindCertificateInStore), XBEM_HLE_STUB(CertFindChainInStore),
      XBEM_HLE(CertFreeCertificateChain), XBEM_HLE(CertFreeCertificateContext), XBEM_HLE(CryptStringToBinaryW),
  };
  return m;
}

}  // namespace xbem::hle
