#pragma once

// The GDK runtime's XAsync machinery, shared by the API families bound
// through QueryApiImpl (see xgameruntime.cpp). An XAsyncBlock the title owns
// is tracked by its address; operations either run a guest provider
// (XAsyncBegin/XAsyncRun) or are completed by the emulator itself (the
// family functions such as XUserAddAsync, "host operations").

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <utility>

#include "hle/guest_types.h"
#include "hle/probe_com.h"

namespace xbem::hle {

constexpr win::HRESULT kEPending = static_cast<win::HRESULT>(0x8000000A);

// Starts a host operation on `block` (an XAsyncBlock*) and completes it at
// once with `status`; `size` bytes of result are produced by `write` when
// the title reads the result. The completion callback runs on the block's
// queue.
win::HRESULT xasync_run_host(uint64_t block, const void* identity, win::HRESULT status, size_t size,
                             std::function<void(void* buffer)> write);

// XAsyncGetResult for a host operation (the families' *Result functions).
win::HRESULT xasync_result(uint64_t block, const void* identity, size_t size, void* buffer, size_t* used);

// Status of the operation on `block` (E_PENDING while it runs).
win::HRESULT xasync_status(uint64_t block, bool wait);

// A GDK family vtable: IUnknown in slots 0-2, `slots` typed, every other slot
// logs and fails with E_NOTIMPL.
ProbeVtable gdk_vtable(std::initializer_list<std::pair<int, void*>> slots);

// The XUser family object for `iid` (xuser.cpp); null for an unknown interface.
void* xuser_api(const uint8_t iid[16]);

}  // namespace xbem::hle
