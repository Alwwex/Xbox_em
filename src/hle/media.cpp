// Media Foundation (video playback), GameInput and XAudio2.
//
// None of these have a host implementation yet. Entry points fail cleanly
// with E_NOTIMPL (and null out-pointers), which titles treat as "feature not
// available"; every call is logged with its arguments and call site so the
// order in which a title needs them is visible in its run log.

#include <chrono>

#include "hle/discovery.h"
#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;

HRESULT not_available(void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL;
}

// ---------------------------------------------------------------- mfplat

HRESULT XBEM_GUEST_ABI hle_MFStartup(DWORD version, DWORD flags) {
  XBEM_INFO("media", "MFStartup(version {:#x}, flags {:#x}): Media Foundation is not emulated, playback will fail",
            version, flags);
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_MFShutdown() { return S_OK; }

int64_t XBEM_GUEST_ABI hle_MFGetSystemTime() {  // 100 ns units
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100;
}

HRESULT XBEM_GUEST_ABI hle_MFCreateAttributes(void** out, uint32_t size) {
  XBEM_LOG_CALL("media", "E_NOTIMPL", reinterpret_cast<uint64_t>(out), size);
  return not_available(out);
}
HRESULT XBEM_GUEST_ABI hle_MFCreateMediaType(void** out) {
  XBEM_LOG_CALL("media", "E_NOTIMPL", reinterpret_cast<uint64_t>(out));
  return not_available(out);
}
HRESULT XBEM_GUEST_ABI hle_MFCreateSourceResolver(void** out) {
  XBEM_LOG_CALL("media", "E_NOTIMPL", reinterpret_cast<uint64_t>(out));
  return not_available(out);
}
HRESULT XBEM_GUEST_ABI hle_MFCreateSourceReaderFromMediaSource(void* source, void* attributes, void** out) {
  XBEM_LOG_CALL("media", "E_NOTIMPL", reinterpret_cast<uint64_t>(source), reinterpret_cast<uint64_t>(attributes),
                reinterpret_cast<uint64_t>(out));
  return not_available(out);
}
HRESULT XBEM_GUEST_ABI hle_MFCreateSourceReaderFromURL(const char16_t* url, void*, void** out) {
  XBEM_WARN("media", "MFCreateSourceReaderFromURL({}) -> E_NOTIMPL", url ? narrow(url) : "");
  return not_available(out);
}
HRESULT XBEM_GUEST_ABI hle_MFCreateSourceReaderFromByteStream(void*, void*, void** out) {
  XBEM_WARN_ONCE("media", "MFCreateSourceReaderFromByteStream -> E_NOTIMPL");
  return not_available(out);
}

// ------------------------------------------------------------- GameInput

// HRESULT GameInputCreate(IGameInput** gameInput)  (gameinput.dll ordinal 1)
HRESULT XBEM_GUEST_ABI hle_GameInputCreate(void** out, uint64_t a1, uint64_t a2, uint64_t a3) {
  XBEM_LOG_CALL("input", "E_NOTIMPL (no input devices yet)", reinterpret_cast<uint64_t>(out), a1, a2, a3);
  return not_available(out);
}

// ---------------------------------------------------------------- XAudio2

// HRESULT XAudio2Create(IXAudio2** out, UINT32 flags, XAUDIO2_PROCESSOR)  (xaudio2_9.dll ordinal 1)
HRESULT XBEM_GUEST_ABI hle_XAudio2Create(void** out, uint64_t flags, uint64_t processor, uint64_t a3) {
  XBEM_LOG_CALL("audio", "E_NOTIMPL (no audio output yet)", reinterpret_cast<uint64_t>(out), flags, processor, a3);
  return not_available(out);
}
HRESULT XBEM_GUEST_ABI hle_XAudio2CreateWithVersionInfo(void** out, uint64_t flags, uint64_t processor, uint64_t ver) {
  XBEM_LOG_CALL("audio", "E_NOTIMPL (no audio output yet)", reinterpret_cast<uint64_t>(out), flags, processor, ver);
  return not_available(out);
}

}  // namespace

std::vector<ModuleDef> make_media_modules() {
  std::vector<ModuleDef> out;
  {
    ModuleDef m;
    m.name = "mfplat.dll";
    m.description = "Media Foundation platform (not emulated: creation functions fail)";
    m.exports = {
        XBEM_HLE(MFStartup),           XBEM_HLE(MFShutdown),           XBEM_HLE(MFGetSystemTime),
        XBEM_HLE_STUB(MFCreateAttributes), XBEM_HLE_STUB(MFCreateMediaType), XBEM_HLE_STUB(MFCreateSourceResolver),
    };
    out.push_back(std::move(m));
  }
  {
    ModuleDef m;
    m.name = "mfreadwrite.dll";
    m.description = "Media Foundation source reader (not emulated)";
    m.exports = {
        XBEM_HLE_STUB(MFCreateSourceReaderFromMediaSource),
        XBEM_HLE_STUB(MFCreateSourceReaderFromURL),
        XBEM_HLE_STUB(MFCreateSourceReaderFromByteStream),
    };
    out.push_back(std::move(m));
  }
  {
    ModuleDef m;
    m.name = "gameinput.dll";
    m.description = "GameInput (not emulated yet)";
    m.exports = {XBEM_HLE_STUB_ORD(GameInputCreate, 1)};
    out.push_back(std::move(m));
  }
  {
    ModuleDef m;
    m.name = "xaudio2_9.dll";
    m.description = "XAudio2 (not emulated yet)";
    m.exports = {XBEM_HLE_STUB_ORD(XAudio2Create, 1), XBEM_HLE_STUB(XAudio2CreateWithVersionInfo)};
    out.push_back(std::move(m));
  }
  return out;
}

}  // namespace xbem::hle
