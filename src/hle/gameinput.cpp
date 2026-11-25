// GameInput (gameinput.dll): the input API of the GDK.
//
// GameInputCreate hands out an IGameInput object with one gamepad connected
// (an Xbox controller, wired). Its state comes from `--press` (scripted
// button presses, for runs without a person) and from the run window's
// keyboard; device callbacks report it on registration, reading callbacks
// fire when its state changes and readings are snapshots of it.
//
// Method order follows the public GameInput.h (v0) interface. Every method
// logs its first calls, so a mismatch with what a title expects shows up in
// the run log immediately.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/gameinput.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

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

// ---------------------------------------------------------- the gamepad
//
// GameInput.h (v0) layouts: GameInputDeviceInfo is 320 bytes (counts up to
// +0x9c, then 20 pointers); GameInputGamepadState is the button mask and six
// floats.

constexpr uint32_t kKindController = 0xE;
constexpr uint32_t kKindGamepad = 0x40000;
constexpr uint32_t kKindUiNavigation = 0x1000000;
constexpr uint32_t kGamepadKinds = kKindController | kKindGamepad | kKindUiNavigation;
constexpr uint32_t kStatusConnected = 0x1, kStatusInputEnabled = 0x2, kStatusOutputEnabled = 0x4;
constexpr uint32_t kGamepadStatus = kStatusConnected | kStatusInputEnabled | kStatusOutputEnabled;

struct GameInputString {
  uint32_t size_in_bytes;
  uint32_t code_point_count;
  const char* data;
  const uint32_t* code_points;
};

struct GameInputDeviceInfo {
  uint32_t info_size;
  uint16_t vendor_id, product_id, revision_number;
  uint8_t interface_number, collection_number;
  uint16_t usage_page, usage_id;
  uint16_t hardware_version[4];
  uint16_t firmware_version[4];
  uint8_t device_id[32];
  uint8_t device_root_id[32];
  uint32_t device_family;
  uint32_t capabilities;
  uint32_t supported_input;
  uint32_t supported_rumble_motors;
  uint32_t input_report_count, output_report_count, feature_report_count;
  uint32_t controller_axis_count, controller_button_count, controller_switch_count;
  uint32_t touch_point_count, touch_sensor_count;
  uint32_t force_feedback_motor_count, haptic_feedback_motor_count;
  uint32_t device_string_count, device_descriptor_size;
  const void* input_report_info;
  const void* output_report_info;
  const void* feature_report_info;
  const void* controller_axis_info;
  const void* controller_button_info;
  const void* controller_switch_info;
  const void* keyboard_info;
  const void* mouse_info;
  const void* touch_sensor_info;
  const void* motion_info;
  const void* arcade_stick_info;
  const void* flight_stick_info;
  const void* gamepad_info;
  const void* racing_wheel_info;
  const void* ui_navigation_info;
  const void* force_feedback_motor_info;
  const void* haptic_feedback_motor_info;
  const GameInputString* display_name;
  const GameInputString* device_strings;
  const void* device_descriptor_data;
};
static_assert(sizeof(GameInputDeviceInfo) == 320);

struct GameInputGamepadState {
  uint32_t buttons;
  float left_trigger, right_trigger;
  float left_thumbstick_x, left_thumbstick_y;
  float right_thumbstick_x, right_thumbstick_y;
};

// GameInputGamepadInfo / GameInputUiNavigationInfo: button labels (zero:
// GameInputLabelNone), large enough for either.
const uint32_t g_labels[32] = {};
const char g_display_name_text[] = "Xbox Wireless Controller";
const GameInputString g_display_name{sizeof g_display_name_text - 1, sizeof g_display_name_text - 1,
                                     g_display_name_text, nullptr};

GameInputDeviceInfo make_device_info() {
  GameInputDeviceInfo info{};
  info.info_size = sizeof info;
  info.vendor_id = 0x045e;   // Microsoft
  info.product_id = 0x0b12;  // Xbox Series X|S controller
  info.usage_page = 0x01;    // generic desktop
  info.usage_id = 0x05;      // game pad
  info.hardware_version[0] = info.firmware_version[0] = 5;
  for (int i = 0; i < 32; ++i) info.device_id[i] = info.device_root_id[i] = static_cast<uint8_t>(0xb0 + i);
  info.device_family = 1;  // GameInputFamilyXboxOne
  info.supported_input = kGamepadKinds;
  info.supported_rumble_motors = 0xf;
  info.controller_axis_count = 6;
  info.controller_button_count = 14;
  info.gamepad_info = g_labels;
  info.ui_navigation_info = g_labels;
  info.display_name = &g_display_name;
  return info;
}
const GameInputDeviceInfo g_device_info = make_device_info();

// The gamepad's current state; `sequence` counts its changes.
std::mutex g_pad_mutex;
std::condition_variable g_pad_changed;
GameInputGamepadState g_pad{};
uint64_t g_pad_sequence = 1;
uint64_t g_pad_timestamp = 0;

void set_pad_buttons(uint32_t buttons, bool down) {
  {
    std::lock_guard lock(g_pad_mutex);
    const uint32_t before = g_pad.buttons;
    g_pad.buttons = down ? (before | buttons) : (before & ~buttons);
    if (g_pad.buttons == before) return;
    ++g_pad_sequence;
    g_pad_timestamp = now_us();
  }
  g_pad_changed.notify_all();
}

ComObject g_device{nullptr};

// ---------------------------------------------------------- IGameInputDevice

const GameInputDeviceInfo* XBEM_GUEST_ABI dev_get_device_info(ComObject*) { return &g_device_info; }
uint32_t XBEM_GUEST_ABI dev_get_device_status(ComObject*) { return kGamepadStatus; }
// void GetBatteryState(GameInputBatteryState*): a wired controller
void XBEM_GUEST_ABI dev_get_battery_state(ComObject*, uint8_t* state) {
  if (state) std::memset(state, 0, 20);  // rates and capacities 0, GameInputBatteryNotPresent
}
HRESULT XBEM_GUEST_ABI dev_not_supported(ComObject*) { return E_NOTIMPL; }
bool XBEM_GUEST_ABI dev_false(ComObject*) { return false; }
void XBEM_GUEST_ABI dev_nothing(ComObject*) {}

void* const g_device_vtable[] = {
    reinterpret_cast<void*>(&query_interface),        // 0  IUnknown
    reinterpret_cast<void*>(&add_ref),                // 1
    reinterpret_cast<void*>(&release),                // 2
    reinterpret_cast<void*>(&dev_get_device_info),    // 3  GetDeviceInfo
    reinterpret_cast<void*>(&dev_get_device_status),  // 4  GetDeviceStatus
    reinterpret_cast<void*>(&dev_get_battery_state),  // 5  GetBatteryState
    reinterpret_cast<void*>(&dev_not_supported),      // 6  CreateForceFeedbackEffect
    reinterpret_cast<void*>(&dev_false),              // 7  IsForceFeedbackMotorPoweredOn
    reinterpret_cast<void*>(&dev_nothing),            // 8  SetForceFeedbackMotorGain
    reinterpret_cast<void*>(&dev_not_supported),      // 9  SetHapticMotorState
    reinterpret_cast<void*>(&dev_nothing),            // 10 SetRumbleState
    reinterpret_cast<void*>(&dev_nothing),            // 11 SetInputSynchronizationState
    reinterpret_cast<void*>(&dev_nothing),            // 12 SendInputSynchronizationHint
    reinterpret_cast<void*>(&dev_nothing),            // 13 PowerOff
    reinterpret_cast<void*>(&dev_not_supported),      // 14 CreateRawDeviceReport
    reinterpret_cast<void*>(&dev_not_supported),      // 15 GetRawDeviceFeature
    reinterpret_cast<void*>(&dev_not_supported),      // 16 SetRawDeviceFeature
    reinterpret_cast<void*>(&dev_not_supported),      // 17 SendRawDeviceOutput
    reinterpret_cast<void*>(&dev_not_supported),      // 18 SendRawDeviceOutputWithResponse
    reinterpret_cast<void*>(&dev_not_supported),      // 19 ExecuteRawDeviceIoControl
    reinterpret_cast<void*>(&dev_false),              // 20 AcquireExclusiveRawDeviceAccess
    reinterpret_cast<void*>(&dev_nothing),            // 21 ReleaseExclusiveRawDeviceAccess
};

// --------------------------------------------------------- IGameInputReading
//
// A reading is a snapshot of the gamepad, freed with its last reference.

struct Reading {
  void* const* vtable;
  std::atomic<ULONG> refs{1};
  uint32_t kind;
  uint64_t sequence;
  uint64_t timestamp;
  GameInputGamepadState pad;
};

ULONG XBEM_GUEST_ABI reading_add_ref(Reading* r) { return r->refs.fetch_add(1) + 1; }
ULONG XBEM_GUEST_ABI reading_release(Reading* r) {
  const ULONG n = r->refs.fetch_sub(1) - 1;
  if (n == 0) delete r;
  return n;
}
HRESULT XBEM_GUEST_ABI reading_query_interface(Reading* r, const uint8_t*, void** out) {
  if (!out) return E_POINTER;
  reading_add_ref(r);
  *out = r;
  return S_OK;
}
uint32_t XBEM_GUEST_ABI reading_get_input_kind(Reading* r) { return r->kind; }
uint64_t XBEM_GUEST_ABI reading_get_sequence_number(Reading* r, uint32_t) { return r->sequence; }
uint64_t XBEM_GUEST_ABI reading_get_timestamp(Reading* r) { return r->timestamp; }
void XBEM_GUEST_ABI reading_get_device(Reading*, void** device) {
  if (!device) return;
  add_ref(&g_device);
  *device = &g_device;
}
bool XBEM_GUEST_ABI reading_false(Reading*) { return false; }
uint32_t XBEM_GUEST_ABI reading_zero(Reading*) { return 0; }
uint32_t XBEM_GUEST_ABI reading_axis_count(Reading*) { return 6; }
// uint32_t GetControllerAxisState(uint32_t count, float* state): triggers, then sticks
uint32_t XBEM_GUEST_ABI reading_axis_state(Reading* r, uint32_t count, float* out) {
  const float axes[6] = {r->pad.left_trigger,      r->pad.right_trigger,      r->pad.left_thumbstick_x,
                         r->pad.left_thumbstick_y, r->pad.right_thumbstick_x, r->pad.right_thumbstick_y};
  const uint32_t n = count < 6 ? count : 6;
  for (uint32_t i = 0; out && i < n; ++i) out[i] = axes[i];
  return n;
}
uint32_t XBEM_GUEST_ABI reading_button_count(Reading*) { return 14; }
// uint32_t GetControllerButtonState(uint32_t count, bool* state): one per GameInputGamepadButtons bit
uint32_t XBEM_GUEST_ABI reading_button_state(Reading* r, uint32_t count, bool* out) {
  const uint32_t n = count < 14 ? count : 14;
  for (uint32_t i = 0; out && i < n; ++i) out[i] = (r->pad.buttons >> i) & 1;
  return n;
}
bool XBEM_GUEST_ABI reading_gamepad_state(Reading* r, GameInputGamepadState* out) {
  if (!out) return false;
  *out = r->pad;
  return true;
}
// GameInputUiNavigationState: up, down, left, right, accept, cancel, ... from the pad
bool XBEM_GUEST_ABI reading_ui_navigation_state(Reading* r, uint32_t* out) {
  if (!out) return false;
  const uint32_t b = r->pad.buttons;
  uint32_t ui = 0;
  if (b & kGamepadMenu) ui |= 0x1;        // Menu
  if (b & kGamepadView) ui |= 0x2;        // View
  if (b & kGamepadA) ui |= 0x4;           // Accept
  if (b & kGamepadB) ui |= 0x8;           // Cancel
  if (b & kGamepadDPadUp) ui |= 0x10;     // Up
  if (b & kGamepadDPadDown) ui |= 0x20;   // Down
  if (b & kGamepadDPadLeft) ui |= 0x40;   // Left
  if (b & kGamepadDPadRight) ui |= 0x80;  // Right
  *out = ui;
  return true;
}

void* const g_reading_vtable[] = {
    reinterpret_cast<void*>(&reading_query_interface),      // 0  IUnknown
    reinterpret_cast<void*>(&reading_add_ref),              // 1
    reinterpret_cast<void*>(&reading_release),              // 2
    reinterpret_cast<void*>(&reading_get_input_kind),       // 3  GetInputKind
    reinterpret_cast<void*>(&reading_get_sequence_number),  // 4  GetSequenceNumber
    reinterpret_cast<void*>(&reading_get_timestamp),        // 5  GetTimestamp
    reinterpret_cast<void*>(&reading_get_device),           // 6  GetDevice
    reinterpret_cast<void*>(&reading_false),                // 7  GetRawReport
    reinterpret_cast<void*>(&reading_axis_count),           // 8  GetControllerAxisCount
    reinterpret_cast<void*>(&reading_axis_state),           // 9  GetControllerAxisState
    reinterpret_cast<void*>(&reading_button_count),         // 10 GetControllerButtonCount
    reinterpret_cast<void*>(&reading_button_state),         // 11 GetControllerButtonState
    reinterpret_cast<void*>(&reading_zero),                 // 12 GetControllerSwitchCount
    reinterpret_cast<void*>(&reading_zero),                 // 13 GetControllerSwitchState
    reinterpret_cast<void*>(&reading_zero),                 // 14 GetKeyCount
    reinterpret_cast<void*>(&reading_zero),                 // 15 GetKeyState
    reinterpret_cast<void*>(&reading_false),                // 16 GetMouseState
    reinterpret_cast<void*>(&reading_zero),                 // 17 GetTouchCount
    reinterpret_cast<void*>(&reading_zero),                 // 18 GetTouchState
    reinterpret_cast<void*>(&reading_false),                // 19 GetMotionState
    reinterpret_cast<void*>(&reading_false),                // 20 GetArcadeStickState
    reinterpret_cast<void*>(&reading_false),                // 21 GetFlightStickState
    reinterpret_cast<void*>(&reading_gamepad_state),        // 22 GetGamepadState
    reinterpret_cast<void*>(&reading_false),                // 23 GetRacingWheelState
    reinterpret_cast<void*>(&reading_ui_navigation_state),  // 24 GetUiNavigationState
};

Reading* snapshot(uint32_t kind) {
  auto* r = new Reading{g_reading_vtable, {1}, 0, 0, 0, {}};
  std::lock_guard lock(g_pad_mutex);
  r->kind = kind & kGamepadKinds ? (kind & kGamepadKinds) : kGamepadKinds;
  r->sequence = g_pad_sequence;
  r->timestamp = g_pad_timestamp ? g_pad_timestamp : now_us();
  r->pad = g_pad;
  return r;
}

// True when a request for `kind` from `device` (null: any) can be served by the gamepad.
bool pad_matches(uint32_t kind, const void* device) {
  return (device == nullptr || device == &g_device) && (kind == 0 || (kind & kGamepadKinds) != 0);
}

// ------------------------------------------------------- callbacks, input thread

using DeviceCallback = void(XBEM_GUEST_ABI*)(uint64_t token, void* context, void* device, uint64_t timestamp,
                                             uint32_t current, uint32_t previous);
using ReadingCallback = void(XBEM_GUEST_ABI*)(uint64_t token, void* context, void* reading, bool overrun);

struct ReadingSubscription {
  uint64_t token, context, callback;
  uint32_t kind;
};
std::mutex g_subscription_mutex;
std::vector<ReadingSubscription> g_reading_callbacks;

std::vector<GamepadPress> g_script;
std::atomic<bool> g_input_thread_started{false};

// A guest thread: runs the --press script and calls the reading callbacks
// when the gamepad changes.
DWORD XBEM_GUEST_ABI input_thread(void*) {
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  struct Event {
    clock::time_point at;
    uint32_t buttons;
    bool down;
  };
  std::vector<Event> events;
  for (const auto& p : g_script) {
    events.push_back({start + std::chrono::milliseconds(p.at_ms), p.buttons, true});
    events.push_back({start + std::chrono::milliseconds(p.at_ms + p.hold_ms), p.buttons, false});
  }
  std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.at < b.at; });
  size_t next = 0;
  uint64_t seen = 0;
  for (;;) {
    {
      std::unique_lock lock(g_pad_mutex);
      const auto until = next < events.size() ? events[next].at : clock::now() + std::chrono::seconds(1);
      g_pad_changed.wait_until(lock, until, [&] { return g_pad_sequence != seen; });
    }
    while (next < events.size() && events[next].at <= clock::now()) {
      XBEM_INFO("input", "gamepad: {} {:#x} (--press)", events[next].down ? "press" : "release", events[next].buttons);
      set_pad_buttons(events[next].buttons, events[next].down);
      ++next;
    }
    uint64_t sequence;
    {
      std::lock_guard lock(g_pad_mutex);
      sequence = g_pad_sequence;
    }
    if (sequence == seen) continue;
    seen = sequence;
    std::vector<ReadingSubscription> subscriptions;
    {
      std::lock_guard lock(g_subscription_mutex);
      subscriptions = g_reading_callbacks;
    }
    for (const auto& s : subscriptions) {
      Reading* r = snapshot(s.kind);
      reinterpret_cast<ReadingCallback>(s.callback)(s.token, reinterpret_cast<void*>(s.context), r, false);
      reading_release(r);
    }
  }
  return 0;
}

void ensure_input_thread() {
  if (g_input_thread_started.exchange(true)) return;
  if (!proc().create_thread(reinterpret_cast<uint64_t>(&input_thread), 0, 0, false))
    XBEM_ERROR("input", "cannot start the input thread");
}

// ------------------------------------------------------------- IGameInput

uint64_t XBEM_GUEST_ABI gi_get_current_timestamp(ComObject*) { return now_us(); }

// HRESULT GetCurrentReading(GameInputKind, IGameInputDevice*, IGameInputReading**)
HRESULT XBEM_GUEST_ABI gi_get_current_reading(ComObject*, uint32_t kind, void* device, void** reading) {
  XBEM_LOG_CALL("input", "the gamepad's state", kind, reinterpret_cast<uint64_t>(device));
  if (!reading) return E_POINTER;
  *reading = nullptr;
  if (!pad_matches(kind, device)) return GAMEINPUT_E_READING_NOT_FOUND;
  *reading = snapshot(kind);
  return S_OK;
}
// HRESULT GetNextReading(IGameInputReading* ref, GameInputKind, IGameInputDevice*, IGameInputReading**):
// only the current state is kept, so the next reading is the current one when it is newer.
HRESULT XBEM_GUEST_ABI gi_get_next_reading(ComObject*, Reading* ref, uint32_t kind, void* device, void** reading) {
  if (!reading) return E_POINTER;
  *reading = nullptr;
  if (!ref || !pad_matches(kind, device)) return GAMEINPUT_E_READING_NOT_FOUND;
  Reading* r = snapshot(kind);
  if (r->sequence <= ref->sequence) {
    reading_release(r);
    return GAMEINPUT_E_READING_NOT_FOUND;
  }
  *reading = r;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI gi_get_previous_reading(ComObject*, void*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND;
}
// HRESULT GetTemporalReading(uint64_t timestamp, IGameInputDevice*, IGameInputReading**)
HRESULT XBEM_GUEST_ABI gi_get_temporal_reading(ComObject*, uint64_t, void* device, void** reading) {
  if (!reading) return E_POINTER;
  *reading = nullptr;
  if (!pad_matches(0, device)) return GAMEINPUT_E_READING_NOT_FOUND;
  *reading = snapshot(kKindGamepad);
  return S_OK;
}

// HRESULT RegisterReadingCallback(IGameInputDevice*, GameInputKind, float analogThreshold, void* context,
//                                 GameInputReadingCallback, GameInputCallbackToken* token)
HRESULT XBEM_GUEST_ABI gi_register_reading_callback(ComObject*, void* device, uint32_t kind, float, void* context,
                                                    uint64_t callback, uint64_t* token) {
  XBEM_LOG_CALL("input", "S_OK (fires on gamepad changes)", reinterpret_cast<uint64_t>(device), kind,
                reinterpret_cast<uint64_t>(context), callback);
  if (!callback) return E_INVALIDARG;
  const uint64_t t = g_next_token.fetch_add(1);
  if (pad_matches(kind, device)) {
    std::lock_guard lock(g_subscription_mutex);
    g_reading_callbacks.push_back({t, reinterpret_cast<uint64_t>(context), callback, kind});
  }
  ensure_input_thread();
  if (token) *token = t;
  return S_OK;
}
// HRESULT RegisterDeviceCallback(IGameInputDevice*, GameInputKind, GameInputDeviceStatus statusFilter,
//                                GameInputEnumerationKind, void* context, GameInputDeviceCallback,
//                                GameInputCallbackToken* token)
// The gamepad is reported at once (blocking and asynchronous enumeration
// alike); it never disconnects.
HRESULT XBEM_GUEST_ABI gi_register_device_callback(ComObject*, void* device, uint32_t kind, uint32_t status_filter,
                                                   uint32_t enumeration, void* context, uint64_t callback,
                                                   uint64_t* token) {
  const bool report = enumeration != 0 && callback && pad_matches(kind, device) && (status_filter & kGamepadStatus);
  XBEM_LOG_CALL("input", report ? "S_OK (the gamepad is reported)" : "S_OK (nothing to report)",
                reinterpret_cast<uint64_t>(device), kind, status_filter, enumeration);
  const uint64_t t = g_next_token.fetch_add(1);
  if (token) *token = t;
  if (report)
    reinterpret_cast<DeviceCallback>(callback)(t, context, &g_device, now_us(), kGamepadStatus, 0);
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
// HRESULT FindDeviceFromId(const APP_LOCAL_DEVICE_ID*, IGameInputDevice**)
HRESULT XBEM_GUEST_ABI gi_find_device_from_id(ComObject*, const uint8_t* id, void** device) {
  if (!device) return E_POINTER;
  *device = nullptr;
  if (!id || std::memcmp(id, g_device_info.device_id, 32) != 0) return GAMEINPUT_E_DEVICE_NOT_FOUND;
  add_ref(&g_device);
  *device = &g_device;
  return S_OK;
}
// HRESULT FindDeviceFrom{Object,PlatformHandle,PlatformString}(..., IGameInputDevice**)
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
    reinterpret_cast<void*>(&gi_find_device_from_id),              // 16 FindDeviceFromId
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
  static std::once_flag once;
  std::call_once(once, [] {
    g_device.vtable = g_device_vtable;
    g_script = parse_gamepad_script(proc().options().press);
    if (!g_script.empty()) ensure_input_thread();
  });
  XBEM_INFO("input", "GameInputCreate: emulated GameInput with one gamepad connected (from {})",
            describe_code_address(XBEM_RETURN_ADDRESS()));
  add_ref(&g_game_input);
  *out = &g_game_input;
  return S_OK;
}

}  // namespace

std::vector<GamepadPress> parse_gamepad_script(const std::string& spec) {
  static const std::pair<const char*, uint32_t> kNames[] = {
      {"MENU", kGamepadMenu},  {"START", kGamepadMenu}, {"VIEW", kGamepadView},       {"A", kGamepadA},
      {"B", kGamepadB},        {"X", kGamepadX},        {"Y", kGamepadY},             {"UP", kGamepadDPadUp},
      {"DOWN", kGamepadDPadDown}, {"LEFT", kGamepadDPadLeft}, {"RIGHT", kGamepadDPadRight},
      {"LB", kGamepadLeftShoulder}, {"RB", kGamepadRightShoulder}, {"LS", kGamepadLeftThumbstick},
      {"RS", kGamepadRightThumbstick}};
  std::vector<GamepadPress> out;
  size_t start = 0;
  while (start < spec.size()) {
    const size_t comma = std::min(spec.find(',', start), spec.size());
    const std::string item = spec.substr(start, comma - start);
    start = comma + 1;
    if (item.empty()) continue;
    const size_t at = item.find('@');
    if (at == std::string::npos) throw std::runtime_error("--press " + item + ": expected BUTTONS@SECONDS");
    GamepadPress p;
    for (size_t b = 0; b < at;) {
      const size_t plus = std::min(item.find('+', b), at);
      std::string name = item.substr(b, plus - b);
      for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      uint32_t bit = 0;
      for (const auto& [n, v] : kNames)
        if (name == n) bit = v;
      if (!bit) throw std::runtime_error("--press: unknown button " + name);
      p.buttons |= bit;
      b = plus + 1;
    }
    const std::string when = item.substr(at + 1);
    const size_t colon = when.find(':');
    try {
      p.at_ms = static_cast<uint32_t>(std::stod(when.substr(0, colon)) * 1000.0);
      if (colon != std::string::npos) p.hold_ms = static_cast<uint32_t>(std::stoul(when.substr(colon + 1)));
    } catch (const std::exception&) {
      throw std::runtime_error("--press " + item + ": bad time");
    }
    out.push_back(p);
  }
  return out;
}

void gamepad_set_buttons(uint32_t buttons, bool down) { set_pad_buttons(buttons, down); }

void gamepad_key(unsigned long keysym, bool down) {
  uint32_t button = 0;
  switch (keysym) {
    case 0xff0d: case 0x20: button = kGamepadA; break;               // Return, space
    case 0xff08: button = kGamepadB; break;                          // BackSpace
    case 'x': button = kGamepadX; break;
    case 'y': case 'z': button = kGamepadY; break;                   // QWERTZ keyboards
    case 0xff1b: button = kGamepadMenu; break;                       // Escape
    case 0xff09: button = kGamepadView; break;                       // Tab
    case 0xff52: case 'w': button = kGamepadDPadUp; break;
    case 0xff54: case 's': button = kGamepadDPadDown; break;
    case 0xff51: case 'a': button = kGamepadDPadLeft; break;
    case 0xff53: case 'd': button = kGamepadDPadRight; break;
    case 'q': button = kGamepadLeftShoulder; break;
    case 'e': button = kGamepadRightShoulder; break;
    default: return;
  }
  set_pad_buttons(button, down);
}

ModuleDef make_gameinput_module() {
  ModuleDef m;
  m.name = "gameinput.dll";
  m.description = "GameInput: one gamepad, driven by --press scripts and the run window keyboard";
  m.exports = {XBEM_HLE_ORD(GameInputCreate, 1)};
  return m;
}

}  // namespace xbem::hle
