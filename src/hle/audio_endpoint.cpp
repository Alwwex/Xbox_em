// Audio endpoints: the Windows Core Audio interfaces a console title renders
// through - MMDeviceEnumerator, one render device, IAudioClient's mix format
// and Spatial Audio (Windows Sonic) object streams.
//
// Nothing is played. Every stream started with an event handle has its event
// signaled every 10 ms (480 frames at 48 kHz, the console's spatial audio
// buffer), so the title's audio thread runs its mixer at the hardware's pace
// and hands each object a zeroed buffer.
//
// GTA VI's audio start-up (fn 0x43aea20 / 0x1e7f580) needs these: without a
// device it falls back to a "fake audio mixer" in which fn 0x5267720 takes
// the mixer lock and returns without releasing it when no output stream
// exists, deadlocking the audio thread and slowing the main loop to its
// 100 ms timeout per frame.
//
// Method order follows the public Windows SDK headers (mmdeviceapi.h,
// audioclient.h, spatialaudioclient.h); all interfaces start with IUnknown.

#include "hle/audio_endpoint.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/strings.h"
#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "platform/memory.h"
#include "runtime/objects.h"

namespace xbem::hle {
namespace {

using namespace win;

constexpr HRESULT E_NOINTERFACE_ = static_cast<HRESULT>(0x80004002);
constexpr HRESULT E_NOTFOUND = static_cast<HRESULT>(0x80070490);  // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFramesPerBuffer = 480;  // 10 ms
constexpr uint32_t kMixChannels = 2;
constexpr uint32_t kNativeStaticMask = 0x1ffe;  // 7.1.4: front L/R/C, LFE, side, back, 4 top

// GUIDs as they are laid out in memory.
constexpr uint8_t kClsidMMDeviceEnumerator[16] = {0x95, 0x03, 0xde, 0xbc, 0x2f, 0xe5, 0x7c, 0x46,
                                                  0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e};
constexpr uint8_t kIidAudioClient[16] = {0x4c, 0xad, 0xb9, 0x1c, 0xfa, 0xdb, 0x32, 0x4c,
                                         0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2};
constexpr uint8_t kIidSpatialAudioClient[16] = {0x66, 0xe0, 0xf8, 0xbb, 0xaa, 0xaa, 0xbe, 0x49,
                                                0x9a, 0x4d, 0xfd, 0x2a, 0x85, 0x8e, 0xa2, 0x7f};
constexpr uint8_t kKsDataFormatIeeeFloat[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

bool same_guid(const void* a, const uint8_t b[16]) {
  uint8_t g[16];
  return a && platform::safe_read(reinterpret_cast<uint64_t>(a), g, 16) && std::memcmp(g, b, 16) == 0;
}

std::string guid_arg(const void* p) {
  uint8_t g[16] = {};
  if (!p || !platform::safe_read(reinterpret_cast<uint64_t>(p), g, 16)) return "null";
  return "{" + guid_to_string(g) + "}";
}

#pragma pack(push, 1)
struct WaveFormatEx {
  uint16_t format_tag, channels;
  uint32_t samples_per_sec, avg_bytes_per_sec;
  uint16_t block_align, bits_per_sample, cb_size;
};
struct WaveFormatExtensible {
  WaveFormatEx format;
  uint16_t valid_bits;
  uint32_t channel_mask;
  uint8_t sub_format[16];
};
#pragma pack(pop)
static_assert(sizeof(WaveFormatEx) == 18 && sizeof(WaveFormatExtensible) == 40);

// The spatial audio object format: 48 kHz mono float.
const WaveFormatEx& object_format() {
  static WaveFormatEx* f = [] {
    auto* p = static_cast<WaveFormatEx*>(heap_alloc(sizeof(WaveFormatEx), true));
    *p = {3, 1, kSampleRate, kSampleRate * 4, 4, 32, 0};
    return p;
  }();
  return *f;
}

template <class F>
void* fn(F f) {
  return reinterpret_cast<void*>(f);
}

HRESULT hand_out(void* object, void** out) {
  if (!out) return E_POINTER;
  *out = object;
  return S_OK;
}

// ---- the event clock ---------------------------------------------------------

struct Stream {
  std::shared_ptr<rt::EventObject> event;
  bool running = false;
  uint32_t static_mask = 0;
  std::vector<uint8_t*> buffers;  // one per activated object
};

class EventClock {
 public:
  static EventClock& get() {
    static EventClock* clock = new EventClock;  // never destroyed: the thread runs to exit
    return *clock;
  }
  void add(Stream* s) {
    std::lock_guard g(lock_);
    streams_.push_back(s);
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    cv_.notify_all();
  }

 private:
  void run() {
    auto next = std::chrono::steady_clock::now();
    std::unique_lock g(lock_);
    for (;;) {
      next += std::chrono::milliseconds(10);
      cv_.wait_until(g, next);
      const auto now = std::chrono::steady_clock::now();
      if (now - next > std::chrono::milliseconds(100)) next = now;  // stalled (debugger, suspend): no burst
      for (Stream* s : streams_)
        if (s->running && s->event) s->event->set();
    }
  }
  std::mutex lock_;
  std::condition_variable cv_;
  std::vector<Stream*> streams_;
  std::thread thread_;
};

// ---- ISpatialAudioObject -----------------------------------------------------

struct AudioObject {
  uint32_t type = 0;
  uint8_t* buffer = nullptr;
  uint32_t bytes = 0;
};

AudioObject* object_state(ProbeObject* self) { return static_cast<AudioObject*>(self->state); }

// 3 GetBuffer(BYTE** buffer, UINT32* bufferLength)
HRESULT XBEM_GUEST_ABI object_get_buffer(ProbeObject* self, uint8_t** buffer, uint32_t* length) {
  AudioObject* o = object_state(self);
  std::memset(o->buffer, 0, o->bytes);
  if (buffer) *buffer = o->buffer;
  if (length) *length = o->bytes;
  return S_OK;
}
// 4 SetEndOfStream(UINT32 frameCount)
HRESULT XBEM_GUEST_ABI object_set_end_of_stream(ProbeObject*, uint32_t) { return S_OK; }
// 5 IsActive(BOOL*)
HRESULT XBEM_GUEST_ABI object_is_active(ProbeObject*, BOOL* active) {
  if (active) *active = TRUE;
  return S_OK;
}
// 6 GetAudioObjectType(AudioObjectType*)
HRESULT XBEM_GUEST_ABI object_get_type(ProbeObject* self, uint32_t* type) {
  if (type) *type = object_state(self)->type;
  return S_OK;
}
// 7 SetPosition(x, y, z), 8 SetVolume(volume)
HRESULT XBEM_GUEST_ABI object_set_position(ProbeObject*) { return S_OK; }
HRESULT XBEM_GUEST_ABI object_set_volume(ProbeObject*) { return S_OK; }

// ---- ISpatialAudioObjectRenderStream ---------------------------------------

Stream* stream_state(ProbeObject* self) { return static_cast<Stream*>(self->state); }

// 3 GetAvailableDynamicObjectCount(UINT32*)
HRESULT XBEM_GUEST_ABI stream_available_dynamic(ProbeObject*, uint32_t* count) {
  if (count) *count = 0;
  return S_OK;
}
// 4 GetService(riid, void**)
HRESULT XBEM_GUEST_ABI stream_get_service(ProbeObject* self, const void* iid, void** out) {
  probe_log(self, 4, std::format("GetService({}) -> E_NOINTERFACE", guid_arg(iid)), XBEM_RETURN_ADDRESS());
  if (out) *out = nullptr;
  return E_NOINTERFACE_;
}
// 5 Start, 6 Stop, 7 Reset
HRESULT XBEM_GUEST_ABI stream_start(ProbeObject* self) {
  probe_log(self, 5, "Start()", XBEM_RETURN_ADDRESS());
  stream_state(self)->running = true;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI stream_stop(ProbeObject* self) {
  probe_log(self, 6, "Stop()", XBEM_RETURN_ADDRESS());
  stream_state(self)->running = false;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI stream_reset(ProbeObject*) { return S_OK; }
// 8 BeginUpdatingAudioObjects(UINT32* availableDynamicObjectCount, UINT32* frameCountPerBuffer)
HRESULT XBEM_GUEST_ABI stream_begin_update(ProbeObject*, uint32_t* dynamic_count, uint32_t* frames) {
  if (dynamic_count) *dynamic_count = 0;
  if (frames) *frames = kFramesPerBuffer;
  return S_OK;
}
// 9 EndUpdatingAudioObjects()
HRESULT XBEM_GUEST_ABI stream_end_update(ProbeObject*) { return S_OK; }

const ProbeVtable& audio_object_vtable() {
  static const ProbeVtable v = make_probe_vtable({{3, fn(&object_get_buffer)},
                                                  {4, fn(&object_set_end_of_stream)},
                                                  {5, fn(&object_is_active)},
                                                  {6, fn(&object_get_type)},
                                                  {7, fn(&object_set_position)},
                                                  {8, fn(&object_set_volume)}});
  return v;
}

// 10 ActivateSpatialAudioObject(AudioObjectType, ISpatialAudioObject**)
HRESULT XBEM_GUEST_ABI stream_activate_object(ProbeObject* self, uint32_t type, void** out) {
  Stream* s = stream_state(self);
  const bool static_type = type != 0 && (type & (type - 1)) == 0 && (s->static_mask & type);
  const bool dynamic = type == 1;  // AudioObjectType_Dynamic: none available
  probe_log(self, 10,
            std::format("ActivateSpatialAudioObject(type {:#x}) -> {}", type,
                        static_type ? "S_OK" : "SPTLAUDCLNT_E_NO_MORE_OBJECTS / E_INVALIDARG"),
            XBEM_RETURN_ADDRESS());
  if (out) *out = nullptr;
  if (dynamic) return static_cast<HRESULT>(0x88890007);  // SPTLAUDCLNT_E_NO_MORE_OBJECTS
  if (!static_type) return E_INVALIDARG;
  auto* o = new AudioObject;
  o->type = type;
  o->bytes = kFramesPerBuffer * object_format().block_align;
  o->buffer = static_cast<uint8_t*>(heap_alloc(o->bytes, true));
  s->buffers.push_back(o->buffer);
  return hand_out(make_probe_object("audio.spatial_object", &audio_object_vtable(), o), out);
}

const ProbeVtable& stream_vtable() {
  static const ProbeVtable v = make_probe_vtable(
      {{3, fn(&stream_available_dynamic)}, {4, fn(&stream_get_service)}, {5, fn(&stream_start)},
       {6, fn(&stream_stop)}, {7, fn(&stream_reset)}, {8, fn(&stream_begin_update)},
       {9, fn(&stream_end_update)}, {10, fn(&stream_activate_object)}});
  return v;
}

// ---- IAudioFormatEnumerator --------------------------------------------------

// 3 GetCount(UINT32*)
HRESULT XBEM_GUEST_ABI formats_count(ProbeObject*, uint32_t* count) {
  if (count) *count = 1;
  return S_OK;
}
// 4 GetFormat(UINT32 index, WAVEFORMATEX** format): the enumerator keeps the format
HRESULT XBEM_GUEST_ABI formats_get(ProbeObject*, uint32_t index, const WaveFormatEx** format) {
  if (!format) return E_POINTER;
  if (index != 0) return E_INVALIDARG;
  *format = &object_format();
  return S_OK;
}

// ---- ISpatialAudioClient -----------------------------------------------------

// Unit-length speaker directions (x right, y up, z back) for the static
// object types, by bit index of AudioObjectType.
void static_position(uint32_t type, float& x, float& y, float& z) {
  struct P {
    uint32_t type;
    float x, y, z;
  };
  static const P kPositions[] = {
      {0x2, -0.5f, 0, -0.866f},      {0x4, 0.5f, 0, -0.866f},        {0x8, 0, 0, -1},
      {0x10, 0, 0, -1},              {0x20, -1, 0, 0},               {0x40, 1, 0, 0},
      {0x80, -0.5f, 0, 0.866f},      {0x100, 0.5f, 0, 0.866f},       {0x200, -0.5f, 0.707f, -0.5f},
      {0x400, 0.5f, 0.707f, -0.5f},  {0x800, -0.5f, 0.707f, 0.5f},   {0x1000, 0.5f, 0.707f, 0.5f},
      {0x2000, -0.5f, -0.707f, -0.5f}, {0x4000, 0.5f, -0.707f, -0.5f}, {0x8000, -0.5f, -0.707f, 0.5f},
      {0x10000, 0.5f, -0.707f, 0.5f}, {0x20000, 0, 0, 1}};
  x = y = z = 0;
  for (const P& p : kPositions)
    if (p.type == type) {
      x = p.x;
      y = p.y;
      z = p.z;
    }
}

// 3 GetStaticObjectPosition(type, float* x, float* y, float* z)
HRESULT XBEM_GUEST_ABI spatial_static_position(ProbeObject*, uint32_t type, float* x, float* y, float* z) {
  if (!x || !y || !z) return E_POINTER;
  if (type == 0 || (type & (type - 1)) || type == 1) return E_INVALIDARG;
  static_position(type, *x, *y, *z);
  return S_OK;
}
// 4 GetNativeStaticObjectTypeMask(AudioObjectType*)
HRESULT XBEM_GUEST_ABI spatial_native_mask(ProbeObject*, uint32_t* mask) {
  if (mask) *mask = kNativeStaticMask;
  return S_OK;
}
// 5 GetMaxDynamicObjectCount(UINT32*)
HRESULT XBEM_GUEST_ABI spatial_max_dynamic(ProbeObject*, uint32_t* count) {
  if (count) *count = 0;
  return S_OK;
}
// 6 GetSupportedAudioObjectFormatEnumerator(IAudioFormatEnumerator**)
HRESULT XBEM_GUEST_ABI spatial_formats(ProbeObject*, void** out) {
  static const ProbeVtable v = make_probe_vtable({{3, fn(&formats_count)}, {4, fn(&formats_get)}});
  return hand_out(make_probe_object("audio.format_enumerator", &v), out);
}
// 7 GetMaxFrameCount(const WAVEFORMATEX*, UINT32*)
HRESULT XBEM_GUEST_ABI spatial_max_frames(ProbeObject*, const void*, uint32_t* frames) {
  if (frames) *frames = kFramesPerBuffer;
  return S_OK;
}
// 8 IsAudioObjectFormatSupported(const WAVEFORMATEX*)
HRESULT XBEM_GUEST_ABI spatial_format_supported(ProbeObject*, const WaveFormatEx* f) {
  if (!f) return E_POINTER;
  return f->samples_per_sec == kSampleRate && f->channels == 1 ? S_OK : static_cast<HRESULT>(0x88890008);
}
// 9 IsSpatialAudioStreamAvailable(riid, const PROPVARIANT*)
HRESULT XBEM_GUEST_ABI spatial_stream_available(ProbeObject*, const void*, const void*) { return S_OK; }

// 10 ActivateSpatialAudioStream(const PROPVARIANT* params, riid, void** stream).
// params: VT_BLOB holding SpatialAudioObjectRenderStreamActivationParams
// {ObjectFormat, StaticObjectTypeMask, Min/MaxDynamicObjectCount, Category,
// EventHandle, NotifyObject}.
HRESULT XBEM_GUEST_ABI spatial_activate_stream(ProbeObject* self, const uint8_t* params, const void* iid, void** out) {
  if (out) *out = nullptr;
  if (!params || !out) return E_POINTER;
  uint16_t vt = 0;
  uint32_t blob_size = 0;
  uint64_t blob = 0;
  std::memcpy(&vt, params, 2);
  std::memcpy(&blob_size, params + 8, 4);
  std::memcpy(&blob, params + 16, 8);
  if (vt != 0x41 || blob_size < 0x28 || !blob) return E_INVALIDARG;
  uint8_t p[0x28];
  if (!platform::safe_read(blob, p, sizeof p)) return E_INVALIDARG;
  uint32_t mask, min_dynamic, max_dynamic, category;
  uint64_t event_handle;
  std::memcpy(&mask, p + 8, 4);
  std::memcpy(&min_dynamic, p + 12, 4);
  std::memcpy(&max_dynamic, p + 16, 4);
  std::memcpy(&category, p + 20, 4);
  std::memcpy(&event_handle, p + 24, 8);
  auto* s = new Stream;
  s->static_mask = mask & kNativeStaticMask;
  if (event_handle) s->event = proc().handles().get_as<rt::EventObject>(event_handle);
  probe_log(self, 10,
            std::format("ActivateSpatialAudioStream({}, static mask {:#x}, dynamic {}..{}, category {}, event {:#x}{})",
                        guid_arg(iid), mask, min_dynamic, max_dynamic, category, event_handle,
                        s->event ? "" : " - not an event"),
            XBEM_RETURN_ADDRESS());
  if (min_dynamic > 0) {
    delete s;
    return static_cast<HRESULT>(0x88890007);  // SPTLAUDCLNT_E_NO_MORE_OBJECTS: no dynamic objects
  }
  EventClock::get().add(s);
  *out = make_probe_object("audio.spatial_stream", &stream_vtable(), s);
  return S_OK;
}

// ---- IAudioClient -----------------------------------------------------------

// 8 GetMixFormat(WAVEFORMATEX**): CoTaskMemAlloc'ed, freed by the caller
HRESULT XBEM_GUEST_ABI client_mix_format(ProbeObject*, WaveFormatExtensible** out) {
  if (!out) return E_POINTER;
  auto* f = static_cast<WaveFormatExtensible*>(heap_alloc(sizeof(WaveFormatExtensible), true));
  f->format = {0xfffe, kMixChannels, kSampleRate, kSampleRate * 4 * kMixChannels, 4 * kMixChannels, 32, 22};
  f->valid_bits = 32;
  f->channel_mask = 0x3;  // front left, front right
  std::memcpy(f->sub_format, kKsDataFormatIeeeFloat, 16);
  *out = f;
  return S_OK;
}
// 9 GetDevicePeriod(REFERENCE_TIME* default, REFERENCE_TIME* minimum)
HRESULT XBEM_GUEST_ABI client_device_period(ProbeObject*, int64_t* def, int64_t* minimum) {
  if (def) *def = 100000;  // 10 ms
  if (minimum) *minimum = 30000;
  return S_OK;
}

// ---- IMMDevice, IMMDeviceCollection, IPropertyStore, IMMDeviceEnumerator -----

// 3 Activate(riid, clsctx, PROPVARIANT* params, void** out)
HRESULT XBEM_GUEST_ABI device_activate(ProbeObject* self, const void* iid, uint32_t clsctx, const void*, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (same_guid(iid, kIidAudioClient)) {
    static const ProbeVtable v = make_probe_vtable({{8, fn(&client_mix_format)}, {9, fn(&client_device_period)}});
    probe_log(self, 3, "Activate(IAudioClient)", XBEM_RETURN_ADDRESS());
    return hand_out(make_probe_object("audio.client", &v), out);
  }
  if (same_guid(iid, kIidSpatialAudioClient)) {
    static const ProbeVtable v = make_probe_vtable(
        {{3, fn(&spatial_static_position)}, {4, fn(&spatial_native_mask)}, {5, fn(&spatial_max_dynamic)},
         {6, fn(&spatial_formats)}, {7, fn(&spatial_max_frames)}, {8, fn(&spatial_format_supported)},
         {9, fn(&spatial_stream_available)}, {10, fn(&spatial_activate_stream)}});
    probe_log(self, 3, "Activate(ISpatialAudioClient)", XBEM_RETURN_ADDRESS());
    return hand_out(make_probe_object("audio.spatial_client", &v), out);
  }
  probe_log(self, 3, std::format("Activate({}, clsctx {:#x}) -> E_NOINTERFACE", guid_arg(iid), clsctx),
            XBEM_RETURN_ADDRESS());
  return E_NOINTERFACE_;
}

// IPropertyStore 5 GetValue(const PROPERTYKEY*, PROPVARIANT*): PKEY_AudioEndpoint_FormFactor
// is Speakers (VT_UI4 1); other keys are VT_EMPTY.
HRESULT XBEM_GUEST_ABI store_get_value(ProbeObject*, const uint8_t* key, uint8_t* value) {
  static const uint8_t kFormFactor[16] = {0x03, 0xd8, 0xa5, 0x1d, 0x92, 0xd4, 0xdd, 0x4e,
                                          0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e};
  if (!key || !value) return E_POINTER;
  std::memset(value, 0, 24);
  uint32_t pid = 0;
  std::memcpy(&pid, key + 16, 4);
  if (std::memcmp(key, kFormFactor, 16) == 0 && pid == 0) {
    value[0] = 19;  // VT_UI4
    value[8] = 1;   // Speakers
  }
  return S_OK;
}
// 4 OpenPropertyStore(stgm, IPropertyStore**)
HRESULT XBEM_GUEST_ABI device_property_store(ProbeObject*, uint32_t, void** out) {
  static const ProbeVtable v = make_probe_vtable({{5, fn(&store_get_value)}});
  return hand_out(make_probe_object("audio.property_store", &v), out);
}
// 5 GetId(LPWSTR*): CoTaskMemAlloc'ed
HRESULT XBEM_GUEST_ABI device_get_id(ProbeObject*, char16_t** out) {
  if (!out) return E_POINTER;
  static const char kId[] = "{0.0.0.00000000}.{58424d45-0000-0000-0000-72656e646572}";
  auto* s = static_cast<char16_t*>(heap_alloc(sizeof kId * 2, true));
  for (size_t i = 0; i < sizeof kId; ++i) s[i] = static_cast<char16_t>(kId[i]);
  *out = s;
  return S_OK;
}
// 6 GetState(DWORD*)
HRESULT XBEM_GUEST_ABI device_get_state(ProbeObject*, uint32_t* state) {
  if (state) *state = 1;  // DEVICE_STATE_ACTIVE
  return S_OK;
}

void* render_device() {
  static void* device = [] {
    static const ProbeVtable v = make_probe_vtable({{3, fn(&device_activate)},
                                                    {4, fn(&device_property_store)},
                                                    {5, fn(&device_get_id)},
                                                    {6, fn(&device_get_state)}});
    return make_probe_object("audio.render_device", &v);
  }();
  return device;
}

// IMMDeviceCollection 3 GetCount(UINT*), 4 Item(UINT, IMMDevice**); state = device count
HRESULT XBEM_GUEST_ABI collection_count(ProbeObject* self, uint32_t* count) {
  if (!count) return E_POINTER;
  *count = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self->state));
  return S_OK;
}
HRESULT XBEM_GUEST_ABI collection_item(ProbeObject* self, uint32_t index, void** out) {
  if (out) *out = nullptr;
  if (index >= reinterpret_cast<uintptr_t>(self->state)) return E_INVALIDARG;
  return hand_out(render_device(), out);
}

// 3 EnumAudioEndpoints(EDataFlow, DWORD stateMask, IMMDeviceCollection**)
HRESULT XBEM_GUEST_ABI enumerator_enum(ProbeObject* self, uint32_t flow, uint32_t mask, void** out) {
  const uintptr_t count = (flow == 0 || flow == 2) && (mask & 1) ? 1 : 0;  // eRender / eAll, ACTIVE
  probe_log(self, 3, std::format("EnumAudioEndpoints(flow {}, mask {:#x}) -> {} devices", flow, mask, count),
            XBEM_RETURN_ADDRESS());
  static const ProbeVtable v = make_probe_vtable({{3, fn(&collection_count)}, {4, fn(&collection_item)}});
  return hand_out(make_probe_object("audio.device_collection", &v, reinterpret_cast<void*>(count)), out);
}
// 4 GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice**)
HRESULT XBEM_GUEST_ABI enumerator_default(ProbeObject* self, uint32_t flow, uint32_t role, void** out) {
  probe_log(self, 4, std::format("GetDefaultAudioEndpoint(flow {}, role {})", flow, role), XBEM_RETURN_ADDRESS());
  if (out) *out = nullptr;
  if (flow != 0) return E_NOTFOUND;  // no capture devices
  return hand_out(render_device(), out);
}
// 5 GetDevice(LPCWSTR id, IMMDevice**)
HRESULT XBEM_GUEST_ABI enumerator_get_device(ProbeObject*, const void*, void** out) {
  return hand_out(render_device(), out);
}
// 6 RegisterEndpointNotificationCallback, 7 Unregister...: no device changes
HRESULT XBEM_GUEST_ABI enumerator_notifications(ProbeObject*, void*) { return S_OK; }

}  // namespace

bool create_audio_com_object(const void* clsid, const void* iid, void** out, HRESULT* result) {
  if (!same_guid(clsid, kClsidMMDeviceEnumerator)) return false;
  static const ProbeVtable v = make_probe_vtable({{3, fn(&enumerator_enum)},
                                                  {4, fn(&enumerator_default)},
                                                  {5, fn(&enumerator_get_device)},
                                                  {6, fn(&enumerator_notifications)},
                                                  {7, fn(&enumerator_notifications)}});
  XBEM_INFO("audio", "CoCreateInstance(MMDeviceEnumerator, iid {}): one render device, spatial audio", guid_arg(iid));
  *result = out ? S_OK : E_POINTER;
  if (out) *out = make_probe_object("audio.device_enumerator", &v);
  return true;
}

}  // namespace xbem::hle
