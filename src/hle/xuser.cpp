// XUser: the GDK user family {01acd177-91f9-4763-a38e-ccbb55ce32e0}, bound
// through QueryApiImpl (xgameruntime.cpp). The emulated console has one
// signed-in, offline user that XUserAddAsync returns silently.
//
// The slots follow the order of XUser.h, verified against GTA VI's wrappers
// (argument counts of slots 3-14, 19-25, 33, 34; slot 7 is called as
// XUserAddAsync(AddDefaultUserSilently, &async) and its completion routine
// calls slot 8 with (async, &user)):
//    3 DuplicateHandle  4 CloseHandle  5 Compare  6 GetMaxUsers
//    7 AddAsync  8 AddResult  9 GetLocalId  10 FindUserByLocalId  11 GetId
//   12 FindUserById  13 GetIsGuest  14 GetState  15 GetGamertag
//   16-18 GetGamerPicture{Async,ResultSize,Result}  19 GetAgeGroup
//   20 CheckPrivilege  21/22 ResolvePrivilegeWithUi{Async,Result}
//   23-25 GetTokenAndSignature{Async,ResultSize,Result}  26-28 the Utf16
//   variants  29/30 ResolveIssueWithUi{Async,Result}  31/32 Utf16 variants
//   33 RegisterForChangeEvent  34 UnregisterForChangeEvent
// The second interface {...41120 in GTA VI} holds the newer
// XUserGetGamertag(user, component, size, buffer, &used) in slot 3.

#include <atomic>
#include <cstring>
#include <string_view>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "hle/xasync.h"

namespace xbem::hle {
namespace {

using namespace win;

constexpr HRESULT E_NETWORK_UNREACHABLE = static_cast<HRESULT>(0x800704CF);  // offline
constexpr HRESULT E_NOT_SUFFICIENT_BUFFER = static_cast<HRESULT>(0x8007007A);
constexpr HRESULT E_GAMEUSER_USER_NOT_FOUND = static_cast<HRESULT>(0x89245102);

// The one user. Handles are pointers to it; duplicating counts references only.
struct User {
  uint64_t magic = 0x5245535545425845ull;  // "EXBEUSER"
  uint64_t local_id = 1;
  uint64_t xuid = 0x0009000000000001ull;
  std::atomic<uint32_t> refs{1};
};
User g_user;

User* user_from(uint64_t handle) {
  auto* u = reinterpret_cast<User*>(handle);
  return u == &g_user ? u : nullptr;
}

constexpr std::string_view kGamertag = "xbem";

int g_identity_add;  // XAsync identities of the operations below (addresses only)
int g_identity_privilege;
int g_identity_token;
int g_identity_issue;
int g_identity_picture;

HRESULT XBEM_GUEST_ABI duplicate_handle(ProbeObject*, uint64_t user, uint64_t* out) {
  User* u = user_from(user);
  if (!u || !out) return E_INVALIDARG;
  ++u->refs;
  *out = user;
  return S_OK;
}

void XBEM_GUEST_ABI close_handle(ProbeObject*, uint64_t user) {
  if (User* u = user_from(user)) --u->refs;
}

int32_t XBEM_GUEST_ABI compare(ProbeObject*, uint64_t a, uint64_t b) { return a == b ? 0 : (a < b ? -1 : 1); }

HRESULT XBEM_GUEST_ABI get_max_users(ProbeObject*, uint32_t* out) {
  if (!out) return E_INVALIDARG;
  *out = 4;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI add_async(ProbeObject* self, uint32_t options, uint64_t async) {
  probe_log(self, 7, "XUserAddAsync(options " + std::to_string(options) + ") -> the default user",
            XBEM_RETURN_ADDRESS());
  return xasync_run_host(async, &g_identity_add, S_OK, sizeof(uint64_t), [](void* buffer) {
    ++g_user.refs;
    const uint64_t handle = reinterpret_cast<uint64_t>(&g_user);
    std::memcpy(buffer, &handle, sizeof handle);
  });
}

HRESULT XBEM_GUEST_ABI add_result(ProbeObject*, uint64_t async, uint64_t* user) {
  if (!user) return E_INVALIDARG;
  return xasync_result(async, &g_identity_add, sizeof *user, user, nullptr);
}

HRESULT XBEM_GUEST_ABI get_local_id(ProbeObject*, uint64_t user, uint64_t* out) {
  User* u = user_from(user);
  if (!u || !out) return E_INVALIDARG;
  *out = u->local_id;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI find_user_by_local_id(ProbeObject*, uint64_t local_id, uint64_t* out) {
  if (!out) return E_INVALIDARG;
  if (local_id != g_user.local_id) return E_GAMEUSER_USER_NOT_FOUND;
  ++g_user.refs;
  *out = reinterpret_cast<uint64_t>(&g_user);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI get_id(ProbeObject*, uint64_t user, uint64_t* out) {
  User* u = user_from(user);
  if (!u || !out) return E_INVALIDARG;
  *out = u->xuid;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI find_user_by_id(ProbeObject*, uint64_t xuid, uint64_t* out) {
  if (!out) return E_INVALIDARG;
  if (xuid != g_user.xuid) return E_GAMEUSER_USER_NOT_FOUND;
  ++g_user.refs;
  *out = reinterpret_cast<uint64_t>(&g_user);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI get_is_guest(ProbeObject*, uint64_t user, bool* out) {
  if (!user_from(user) || !out) return E_INVALIDARG;
  *out = false;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI get_state(ProbeObject*, uint64_t user, uint32_t* out) {
  if (!user_from(user) || !out) return E_INVALIDARG;
  *out = 0;  // XUserState::SignedIn
  return S_OK;
}

HRESULT copy_gamertag(size_t size, char* buffer, size_t* used) {
  if (used) *used = kGamertag.size() + 1;
  if (!buffer || size < kGamertag.size() + 1) return E_NOT_SUFFICIENT_BUFFER;
  std::memcpy(buffer, kGamertag.data(), kGamertag.size());
  buffer[kGamertag.size()] = 0;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI get_gamertag(ProbeObject*, uint64_t user, size_t size, char* buffer, size_t* used) {
  if (!user_from(user)) return E_INVALIDARG;
  return copy_gamertag(size, buffer, used);
}

HRESULT XBEM_GUEST_ABI get_gamertag_component(ProbeObject*, uint64_t user, uint32_t, size_t size, char* buffer,
                                              size_t* used) {
  if (!user_from(user)) return E_INVALIDARG;
  return copy_gamertag(size, buffer, used);
}

// No gamer picture: the operation fails like an offline profile service.
HRESULT XBEM_GUEST_ABI get_gamer_picture_async(ProbeObject*, uint64_t user, uint32_t, uint64_t async) {
  if (!user_from(user)) return E_INVALIDARG;
  return xasync_run_host(async, &g_identity_picture, E_NETWORK_UNREACHABLE, 0, nullptr);
}

HRESULT XBEM_GUEST_ABI get_gamer_picture_result_size(ProbeObject*, uint64_t async, size_t* size) {
  const HRESULT hr = xasync_status(async, false);
  if (hr < 0) return hr;
  if (size) *size = 0;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI get_gamer_picture_result(ProbeObject*, uint64_t async, size_t size, void* buffer,
                                                size_t* used) {
  return xasync_result(async, &g_identity_picture, size, buffer, used);
}

HRESULT XBEM_GUEST_ABI get_age_group(ProbeObject*, uint64_t user, uint32_t* out) {
  if (!user_from(user) || !out) return E_INVALIDARG;
  *out = 3;  // XUserAgeGroup::Adult
  return S_OK;
}

// Privileges are granted: the network operations they guard fail anyway.
HRESULT XBEM_GUEST_ABI check_privilege(ProbeObject*, uint64_t user, uint32_t, uint32_t, bool* has,
                                       uint32_t* reasons) {
  if (!user_from(user) || !has) return E_INVALIDARG;
  *has = true;
  if (reasons) *reasons = 0;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI resolve_privilege_async(ProbeObject*, uint64_t user, uint32_t, uint32_t, uint64_t async) {
  if (!user_from(user)) return E_INVALIDARG;
  return xasync_run_host(async, &g_identity_privilege, S_OK, 0, nullptr);
}

HRESULT XBEM_GUEST_ABI resolve_privilege_result(ProbeObject*, uint64_t async) { return xasync_status(async, false); }

// Xbox Live tokens need the network: the operations complete with "unreachable".
HRESULT XBEM_GUEST_ABI token_async(ProbeObject*, uint64_t user, uint32_t, const void*, const void*, size_t,
                                   const void*, size_t, const void*, uint64_t async) {
  if (!user_from(user)) return E_INVALIDARG;
  return xasync_run_host(async, &g_identity_token, E_NETWORK_UNREACHABLE, 0, nullptr);
}

HRESULT XBEM_GUEST_ABI token_result_size(ProbeObject*, uint64_t async, size_t* size) {
  const HRESULT hr = xasync_status(async, false);
  if (hr < 0) return hr;
  if (size) *size = 0;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI token_result(ProbeObject*, uint64_t async, size_t size, void* buffer, void** ptr,
                                    size_t* used) {
  if (ptr) *ptr = nullptr;
  return xasync_result(async, &g_identity_token, size, buffer, used);
}

HRESULT XBEM_GUEST_ABI resolve_issue_async(ProbeObject*, uint64_t user, const void*, uint64_t async) {
  if (!user_from(user)) return E_INVALIDARG;
  return xasync_run_host(async, &g_identity_issue, S_OK, 0, nullptr);
}

HRESULT XBEM_GUEST_ABI resolve_issue_result(ProbeObject*, uint64_t async) { return xasync_status(async, false); }

// The user never changes, so change events are accepted and never raised.
std::atomic<uint64_t> g_next_token{1};

HRESULT XBEM_GUEST_ABI register_change_event(ProbeObject*, uint64_t, void*, uint64_t callback, uint64_t* token) {
  if (!callback || !token) return E_INVALIDARG;
  *token = g_next_token++;
  return S_OK;
}

void XBEM_GUEST_ABI unregister_change_event(ProbeObject*, uint64_t, bool) {}

template <class F>
void* fn(F f) {
  return reinterpret_cast<void*>(f);
}

constexpr uint8_t kXUserIid[16] = {0x77, 0xd1, 0xac, 0x01, 0xf9, 0x91, 0x63, 0x47,
                                   0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0};

}  // namespace

void* xuser_api(const uint8_t iid[16]) {
  if (std::memcmp(iid, kXUserIid, 16) == 0) {
    static const ProbeVtable vtable = gdk_vtable({
        {3, fn(&duplicate_handle)},
        {4, fn(&close_handle)},
        {5, fn(&compare)},
        {6, fn(&get_max_users)},
        {7, fn(&add_async)},
        {8, fn(&add_result)},
        {9, fn(&get_local_id)},
        {10, fn(&find_user_by_local_id)},
        {11, fn(&get_id)},
        {12, fn(&find_user_by_id)},
        {13, fn(&get_is_guest)},
        {14, fn(&get_state)},
        {15, fn(&get_gamertag)},
        {16, fn(&get_gamer_picture_async)},
        {17, fn(&get_gamer_picture_result_size)},
        {18, fn(&get_gamer_picture_result)},
        {19, fn(&get_age_group)},
        {20, fn(&check_privilege)},
        {21, fn(&resolve_privilege_async)},
        {22, fn(&resolve_privilege_result)},
        {23, fn(&token_async)},
        {24, fn(&token_result_size)},
        {25, fn(&token_result)},
        {26, fn(&token_async)},
        {27, fn(&token_result_size)},
        {28, fn(&token_result)},
        {29, fn(&resolve_issue_async)},
        {30, fn(&resolve_issue_result)},
        {31, fn(&resolve_issue_async)},
        {32, fn(&resolve_issue_result)},
        {33, fn(&register_change_event)},
        {34, fn(&unregister_change_event)},
    });
    static void* object = make_probe_object("xgameruntime.XUser", &vtable);
    return object;
  }
  // Any other interface of the family: the newer functions, of which GTA VI
  // uses XUserGetGamertag with a gamertag component in slot 3.
  static const ProbeVtable vtable2 = gdk_vtable({{3, fn(&get_gamertag_component)}});
  static void* object2 = make_probe_object("xgameruntime.XUser2", &vtable2);
  return object2;
}

}  // namespace xbem::hle
