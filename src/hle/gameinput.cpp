// GameInput (gameinput.dll): the input API of the GDK.
//
// GameInputCreate hands out an IGameInput object with no devices connected:
// readings are never found, device enumeration reports nothing, callbacks are
// registered (and never fire) and a dispatcher can be created. That is how a
// console without controllers looks to a title, which lets it get past input
// initialisation. Mapping host keyboard/gamepads to devices comes later.
//
// Method order follows the public GameInput.h (v0) interface. Every method
// logs its first calls, so a mismatch with what a title expects shows up in
// the run log immediately.

#include <atomic>
#include <chrono>

#include "hle/discovery.h"
#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;

constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002);
constexpr HRESULT GAMEINPUT_E_DEVICE_NOT_FOUND = static_cast<HRESULT>(0x838A0001);
constexpr HRESULT GAMEINPUT_E_READING_NOT_FOUND = static_cast<HRESULT>(0x838A0003);



// ---------------------------------------------------------------- objects

struct ComObject {
  void* const* vtable;
  std::atomic<ULONG> refs{1};
};

ULONG XBEM_GUEST_ABI add_ref(ComObject* o) { return o->refs.fetch_add(1) + 1; }
ULONG XBEM_GUEST_ABI release(ComObject* o) {
  const ULONG n = o->refs.fetch_sub(1) - 1;
  // Objects are process-lifetime singletons: never freed (titles may race
  // their final Release with callbacks).
  return n;
}
HRESULT XBEM_GUEST_ABI query_interface(ComObject* o, const uint8_t* iid, void** out) {
  if (!out) return E_POINTER;
  // Any interface of the object resolves to the object itself (the title
  // only asks for IGameInput/IUnknown or the dispatcher interface).
  add_ref(o);
  *out = o;
  static std::atomic<int> logged{0};
  if (iid && logged.fetch_add(1) < 4) XBEM_DEBUG("input", "QueryInterface({{{}}})", guid_to_string(iid));
  return S_OK;
}

std::atomic<uint64_t> g_next_token{1};

uint64_t now_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// ------------------------------------------------------ IGameInputDispatcher

// bool Dispatch(uint64_t quotaInMicroseconds)
bool XBEM_GUEST_ABI dispatcher_dispatch(ComObject*, uint64_t) { return false; }  // nothing pending
// HRESULT OpenWaitHandle(HANDLE* waitHandle)
HRESULT XBEM_GUEST_ABI dispatcher_open_wait_handle(ComObject*, HANDLE* handle) {
  XBEM_LOG_CALL("input", "E_NOTIMPL", reinterpret_cast<uint64_t>(handle));
  if (handle) *handle = 0;
  return E_NOTIMPL;
}

void* const g_dispatcher_vtable[] = {
    reinterpret_cast<void*>(&query_interface),
    reinterpret_cast<void*>(&add_ref),
    reinterpret_cast<void*>(&release),
    reinterpret_cast<void*>(&dispatcher_dispatch),
    reinterpret_cast<void*>(&dispatcher_open_wait_handle),
};
ComObject g_dispatcher{g_dispatcher_vtable};

// ------------------------------------------------------------- IGameInput

uint64_t XBEM_GUEST_ABI gi_get_current_timestamp(ComObject*) { return now_us(); }

// HRESULT GetCurrentReading(GameInputKind, IGameInputDevice*, IGameInputReading**)
HRESULT XBEM_GUEST_ABI gi_get_current_reading(ComObject*, uint32_t kind, void* device, void** reading) {
  XBEM_LOG_CALL("input", "GAMEINPUT_E_READING_NOT_FOUND (no devices)", kind, reinterpret_cast<uint64_t>(device));
  if (reading) *reading = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND;
}
// HRESULT GetNextReading(IGameInputReading* ref, GameInputKind, IGameInputDevice*, IGameInputReading**)
HRESULT XBEM_GUEST_ABI gi_get_next_reading(ComObject*, void*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND;
}
HRESULT XBEM_GUEST_ABI gi_get_previous_reading(ComObject*, void*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND;
}
// HRESULT GetTemporalReading(uint64_t timestamp, IGameInputDevice*, IGameInputReading**)
HRESULT XBEM_GUEST_ABI gi_get_temporal_reading(ComObject*, uint64_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND;
}

// HRESULT RegisterReadingCallback(IGameInputDevice*, GameInputKind, float analogThreshold, void* context,
//                                 GameInputReadingCallback, GameInputCallbackToken* token)
HRESULT XBEM_GUEST_ABI gi_register_reading_callback(ComObject*, void* device, uint32_t kind, float, void* context,
                                                    uint64_t callback, uint64_t* token) {
  XBEM_LOG_CALL("input", "S_OK (never fires)", reinterpret_cast<uint64_t>(device), kind, reinterpret_cast<uint64_t>(context),
         callback);
  if (token) *token = g_next_token.fetch_add(1);
  return S_OK;
}
// HRESULT RegisterDeviceCallback(IGameInputDevice*, GameInputKind, GameInputDeviceStatus statusFilter,
//                                GameInputEnumerationKind, void* context, GameInputDeviceCallback,
//                                GameInputCallbackToken* token)
HRESULT XBEM_GUEST_ABI gi_register_device_callback(ComObject*, void* device, uint32_t kind, uint32_t status_filter,
                                                   uint32_t enumeration, void* context, uint64_t callback,
                                                   uint64_t* token) {
  XBEM_LOG_CALL("input", "S_OK (no devices to enumerate)", reinterpret_cast<uint64_t>(device), kind, status_filter, enumeration);
  (void)context;
  (void)callback;
  if (token) *token = g_next_token.fetch_add(1);
  return S_OK;
}
// HRESULT RegisterSystemButtonCallback(IGameInputDevice*, GameInputSystemButtons, void* context,
//                                      GameInputSystemButtonCallback, GameInputCallbackToken*)
HRESULT XBEM_GUEST_ABI gi_register_system_button_callback(ComObject*, void* device, uint32_t buttons, void*, uint64_t,
                                                          uint64_t* token) {
  XBEM_LOG_CALL("input", "S_OK (never fires)", reinterpret_cast<uint64_t>(device), buttons);
  if (token) *token = g_next_token.fetch_add(1);
  return S_OK;
}
// HRESULT RegisterKeyboardLayoutCallback(IGameInputDevice*, void* context, GameInputKeyboardLayoutCallback,
//                                        GameInputCallbackToken*)
HRESULT XBEM_GUEST_ABI gi_register_keyboard_layout_callback(ComObject*, void*, void*, uint64_t, uint64_t* token) {
  if (token) *token = g_next_token.fetch_add(1);
  return S_OK;
}
void XBEM_GUEST_ABI gi_stop_callback(ComObject*, uint64_t) {}
// bool UnregisterCallback(GameInputCallbackToken, uint64_t timeoutInMicroseconds)
bool XBEM_GUEST_ABI gi_unregister_callback(ComObject*, uint64_t, uint64_t) { return true; }

// HRESULT CreateDispatcher(IGameInputDispatcher**)
HRESULT XBEM_GUEST_ABI gi_create_dispatcher(ComObject*, void** dispatcher) {
  if (!dispatcher) return E_POINTER;
  add_ref(&g_dispatcher);
  *dispatcher = &g_dispatcher;
  return S_OK;
}
// HRESULT CreateAggregateDevice(GameInputKind, IGameInputDevice**)
HRESULT XBEM_GUEST_ABI gi_create_aggregate_device(ComObject*, uint32_t kind, void** device) {
  XBEM_LOG_CALL("input", "E_NOTIMPL", kind);
  if (device) *device = nullptr;
  return E_NOTIMPL;
}
// HRESULT FindDeviceFrom{Id,Object,PlatformHandle,PlatformString}(..., IGameInputDevice**)
HRESULT XBEM_GUEST_ABI gi_find_device(ComObject*, uint64_t, void** device) {
  if (device) *device = nullptr;
  return GAMEINPUT_E_DEVICE_NOT_FOUND;
}
// HRESULT EnableOemDeviceSupport(uint16_t vid, uint16_t pid, uint8_t interface, uint8_t collection)
HRESULT XBEM_GUEST_ABI gi_enable_oem_device_support(ComObject*, uint16_t, uint16_t, uint8_t, uint8_t) { return S_OK; }
// void SetFocusPolicy(GameInputFocusPolicy)
void XBEM_GUEST_ABI gi_set_focus_policy(ComObject*, uint32_t) {}

// Fallback for vtable slots past the known interface (a newer GameInput):
// logged so the layout can be extended.
HRESULT XBEM_GUEST_ABI gi_unknown_method(ComObject*, uint64_t a, uint64_t b, uint64_t c) {
  XBEM_WARN("input", "IGameInput: call to an unknown method ({:#x}, {:#x}, {:#x}) from {} -> E_NOTIMPL", a, b, c,
            describe_code_address(XBEM_RETURN_ADDRESS()));
  return E_NOTIMPL;
}

void* const g_game_input_vtable[] = {
    reinterpret_cast<void*>(&query_interface),                     // 0  IUnknown
    reinterpret_cast<void*>(&add_ref),                             // 1
    reinterpret_cast<void*>(&release),                             // 2
    reinterpret_cast<void*>(&gi_get_current_timestamp),            // 3  GetCurrentTimestamp
    reinterpret_cast<void*>(&gi_get_current_reading),              // 4  GetCurrentReading
    reinterpret_cast<void*>(&gi_get_next_reading),                 // 5  GetNextReading
    reinterpret_cast<void*>(&gi_get_previous_reading),             // 6  GetPreviousReading
    reinterpret_cast<void*>(&gi_get_temporal_reading),             // 7  GetTemporalReading
    reinterpret_cast<void*>(&gi_register_reading_callback),        // 8  RegisterReadingCallback
    reinterpret_cast<void*>(&gi_register_device_callback),         // 9  RegisterDeviceCallback
    reinterpret_cast<void*>(&gi_register_system_button_callback),  // 10 RegisterSystemButtonCallback
    reinterpret_cast<void*>(&gi_register_keyboard_layout_callback),// 11 RegisterKeyboardLayoutCallback
    reinterpret_cast<void*>(&gi_stop_callback),                    // 12 StopCallback
    reinterpret_cast<void*>(&gi_unregister_callback),              // 13 UnregisterCallback
    reinterpret_cast<void*>(&gi_create_dispatcher),                // 14 CreateDispatcher
    reinterpret_cast<void*>(&gi_create_aggregate_device),          // 15 CreateAggregateDevice
    reinterpret_cast<void*>(&gi_find_device),                      // 16 FindDeviceFromId
    reinterpret_cast<void*>(&gi_find_device),                      // 17 FindDeviceFromObject
    reinterpret_cast<void*>(&gi_find_device),                      // 18 FindDeviceFromPlatformHandle
    reinterpret_cast<void*>(&gi_find_device),                      // 19 FindDeviceFromPlatformString
    reinterpret_cast<void*>(&gi_enable_oem_device_support),        // 20 EnableOemDeviceSupport
    reinterpret_cast<void*>(&gi_set_focus_policy),                 // 21 SetFocusPolicy
    reinterpret_cast<void*>(&gi_unknown_method),                   // 22.. (newer versions)
    reinterpret_cast<void*>(&gi_unknown_method),
    reinterpret_cast<void*>(&gi_unknown_method),
    reinterpret_cast<void*>(&gi_unknown_method),
    reinterpret_cast<void*>(&gi_unknown_method),
    reinterpret_cast<void*>(&gi_unknown_method),
    reinterpret_cast<void*>(&gi_unknown_method),
    reinterpret_cast<void*>(&gi_unknown_method),
};
ComObject g_game_input{g_game_input_vtable};

// HRESULT GameInputCreate(IGameInput** gameInput)  (gameinput.dll ordinal 1)
HRESULT XBEM_GUEST_ABI hle_GameInputCreate(void** out) {
  if (!out) return E_POINTER;
  XBEM_INFO("input", "GameInputCreate: emulated GameInput with no devices connected (from {})",
            describe_code_address(XBEM_RETURN_ADDRESS()));
  add_ref(&g_game_input);
  *out = &g_game_input;
  return S_OK;
}

}  // namespace

ModuleDef make_gameinput_module() {
  ModuleDef m;
  m.name = "gameinput.dll";
  m.description = "GameInput: an input system with no devices connected (no host input mapping yet)";
  m.exports = {XBEM_HLE_ORD(GameInputCreate, 1)};
  return m;
}

}  // namespace xbem::hle
