#pragma once

// Diagnostics for console APIs whose exact signatures are not public. Calls
// are logged with every argument (pointers are probed for GUIDs and strings)
// and with the guest call site, so a run of a real title shows how the API is
// used and the implementation can be completed from evidence.

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace xbem::hle {

// "0x1234" plus, for readable pointers, the first 16 bytes as a GUID and any
// printable ASCII/UTF-16 text found there.
std::string describe_arg(uint64_t value);

// "game.exe+0x1234" (or a raw address) for a guest code address.
std::string describe_code_address(uint64_t address);

// For APIs whose return type is unknown (BOOL vs HRESULT): looks at the guest
// code after the call for a sign test (test eax,eax; js/jns), which is how
// SUCCEEDED()/FAILED() compile. Returns the "success" value the caller expects:
// S_OK (0) for HRESULT-style callers, otherwise TRUE (1, also S_FALSE).
int32_t success_value_for_caller(uint64_t return_address);

// The matching "failure" value: E_NOTIMPL for HRESULT-style callers, else 0
// (FALSE / NULL).
int64_t failure_value_for_caller(uint64_t return_address);

void log_api_call(const char* category, const char* function, std::initializer_list<uint64_t> args,
                  uint64_t return_address, std::string_view outcome);

}  // namespace xbem::hle

#if defined(_MSC_VER)
#include <intrin.h>
#define XBEM_RETURN_ADDRESS() reinterpret_cast<uint64_t>(_ReturnAddress())
#else
#define XBEM_RETURN_ADDRESS() reinterpret_cast<uint64_t>(__builtin_return_address(0))
#endif

// Logs the first few calls of the enclosing HLE function (named hle_<Api>).
#define XBEM_LOG_CALL(category, outcome, ...)                                                          \
  do {                                                                                                 \
    static std::atomic<int> xbem_logged_calls{0};                                                      \
    if (xbem_logged_calls.fetch_add(1) < 6)                                                            \
      ::xbem::hle::log_api_call(category, __func__, {__VA_ARGS__}, XBEM_RETURN_ADDRESS(), outcome);   \
  } while (0)

// Defines an entry point of an undocumented console API that logs its first
// calls (four integer arguments) and fails in the way the caller checks for.
#define XBEM_DISCOVERY_FAIL(category, name)                                                       \
  int64_t XBEM_GUEST_ABI hle_##name(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {             \
    const int64_t result = ::xbem::hle::failure_value_for_caller(XBEM_RETURN_ADDRESS());          \
    XBEM_LOG_CALL(category, result ? "E_NOTIMPL (not emulated)" : "0 (not emulated)", a, b, c, d); \
    return result;                                                                                \
  }
