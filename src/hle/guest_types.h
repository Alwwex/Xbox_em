#pragma once

// Types and constants shared by HLE (high-level emulation) implementations of
// the console's system DLLs. Guest code follows the Microsoft x64 calling
// convention; every function the guest can call is marked XBEM_GUEST_ABI.
//
// This header must never be included together with <windows.h>: the Win32
// names below live in xbem::win and would clash with windows.h macros.

#include <cstdint>

#if defined(_WIN32)
#define XBEM_GUEST_ABI
#else
#define XBEM_GUEST_ABI __attribute__((ms_abi))
#endif

namespace xbem::win {

using BOOL = int32_t;
using BOOLEAN = uint8_t;
using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using LONG = int32_t;
using ULONG = uint32_t;
using UINT = uint32_t;
using INT = int32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using SIZE_T = uint64_t;
using ULONG_PTR = uint64_t;
using DWORD_PTR = uint64_t;
using HRESULT = int32_t;
using HANDLE = uint64_t;  // opaque to us; guest pointers and handles are 64-bit
using HMODULE = uint64_t;
using WCHAR = char16_t;
using LPCSTR = const char*;
using LPSTR = char*;
using LPCWSTR = const char16_t*;
using LPWSTR = char16_t*;
using LPVOID = void*;
using LPCVOID = const void*;

constexpr BOOL TRUE = 1;
constexpr BOOL FALSE = 0;

constexpr HANDLE INVALID_HANDLE_VALUE = ~0ull;
constexpr HANDLE kCurrentProcessPseudo = ~0ull;      // GetCurrentProcess()
constexpr HANDLE kCurrentThreadPseudo = ~0ull - 1;   // GetCurrentThread()

constexpr DWORD INFINITE = 0xFFFFFFFF;
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_ABANDONED_0 = 0x80;
constexpr DWORD WAIT_TIMEOUT = 0x102;
constexpr DWORD WAIT_FAILED = 0xFFFFFFFF;
constexpr DWORD STILL_ACTIVE = 259;
constexpr DWORD TLS_OUT_OF_INDEXES = 0xFFFFFFFF;

// Error codes (GetLastError).
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
constexpr DWORD ERROR_PATH_NOT_FOUND = 3;
constexpr DWORD ERROR_ACCESS_DENIED = 5;
constexpr DWORD ERROR_INVALID_HANDLE = 6;
constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr DWORD ERROR_OUTOFMEMORY = 14;
constexpr DWORD ERROR_NO_MORE_FILES = 18;
constexpr DWORD ERROR_NOT_SUPPORTED = 50;
constexpr DWORD ERROR_FILE_EXISTS = 80;
constexpr DWORD ERROR_INVALID_PARAMETER = 87;
constexpr DWORD ERROR_CALL_NOT_IMPLEMENTED = 120;
constexpr DWORD ERROR_INSUFFICIENT_BUFFER = 122;
constexpr DWORD ERROR_MOD_NOT_FOUND = 126;
constexpr DWORD ERROR_PROC_NOT_FOUND = 127;
constexpr DWORD ERROR_ALREADY_EXISTS = 183;
constexpr DWORD ERROR_ENVVAR_NOT_FOUND = 203;
constexpr DWORD ERROR_NOT_OWNER = 288;
constexpr DWORD ERROR_INVALID_ADDRESS = 487;
constexpr DWORD ERROR_TOO_MANY_POSTS = 298;
constexpr DWORD ERROR_TIMEOUT = 1460;

// HRESULTs.
constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001);
constexpr HRESULT E_POINTER = static_cast<HRESULT>(0x80004003);
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000E);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057);
constexpr HRESULT DXGI_ERROR_UNSUPPORTED = static_cast<HRESULT>(0x887A0004);

// Virtual memory.
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD MEM_DECOMMIT = 0x4000;
constexpr DWORD MEM_RELEASE = 0x8000;
constexpr DWORD MEM_FREE = 0x10000;
constexpr DWORD MEM_PRIVATE = 0x20000;
constexpr DWORD MEM_RESET = 0x80000;
constexpr DWORD MEM_TOP_DOWN = 0x100000;
constexpr DWORD MEM_IMAGE = 0x1000000;
constexpr DWORD MEM_LARGE_PAGES = 0x20000000;
constexpr DWORD PAGE_NOACCESS = 0x01;
constexpr DWORD PAGE_READONLY = 0x02;
constexpr DWORD PAGE_READWRITE = 0x04;
constexpr DWORD PAGE_WRITECOPY = 0x08;
constexpr DWORD PAGE_EXECUTE = 0x10;
constexpr DWORD PAGE_EXECUTE_READ = 0x20;
constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;

// DllMain / TLS callback reasons.
constexpr DWORD DLL_PROCESS_DETACH = 0;
constexpr DWORD DLL_PROCESS_ATTACH = 1;
constexpr DWORD DLL_THREAD_ATTACH = 2;
constexpr DWORD DLL_THREAD_DETACH = 3;

// Signatures of guest code we call into.
using GuestDllMain = BOOL(XBEM_GUEST_ABI*)(HMODULE, DWORD, void*);
using GuestTlsCallback = void(XBEM_GUEST_ABI*)(HMODULE, DWORD, void*);
using GuestEntry = DWORD(XBEM_GUEST_ABI*)(void* peb);
using GuestThreadProc = DWORD(XBEM_GUEST_ABI*)(void*);
using GuestFlsCallback = void(XBEM_GUEST_ABI*)(void*);

}  // namespace xbem::win
