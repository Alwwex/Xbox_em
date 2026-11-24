#pragma once

#include "hle/guest_types.h"

namespace xbem::hle {

// CoCreateInstance for the Core Audio classes (MMDeviceEnumerator): true when
// `clsid` is one of them, with the object in *out and the HRESULT in *result.
bool create_audio_com_object(const void* clsid, const void* iid, void** out, win::HRESULT* result);

}  // namespace xbem::hle
