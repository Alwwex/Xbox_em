/* Minimal declarations for guest test programs.
 *
 * These programs are real Windows x64 PE binaries (built with MinGW-w64 and
 * -nostdlib) that import from the same DLL names a console title uses. xbem
 * loads and executes them natively, exercising the loader and HLE layer. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define IMP __declspec(dllimport)

typedef int BOOL;
typedef uint32_t DWORD;
typedef void* HANDLE;
typedef uint16_t WCHAR;
typedef int32_t HRESULT;
typedef DWORD (*THREAD_PROC)(void*);

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
#define PAGE_READWRITE 4u
#define HEAP_ZERO_MEMORY 8u

/* kernelx.dll */
IMP void OutputDebugStringA(const char*);
IMP DWORD GetTickCount(void);
IMP uint64_t GetTickCount64(void);
IMP BOOL QueryPerformanceCounter(int64_t*);
IMP BOOL QueryPerformanceFrequency(int64_t*);
IMP void Sleep(DWORD);
IMP DWORD GetLastError(void);
IMP void SetLastError(DWORD);
IMP DWORD GetCurrentThreadId(void);
IMP HANDLE CreateThread(void*, size_t, THREAD_PROC, void*, DWORD, DWORD*);
IMP DWORD WaitForSingleObject(HANDLE, DWORD);
IMP DWORD WaitForMultipleObjects(DWORD, const HANDLE*, BOOL, DWORD);
IMP BOOL CloseHandle(HANDLE);
IMP BOOL GetExitCodeThread(HANDLE, DWORD*);
IMP void ExitThread(DWORD);
IMP HANDLE CreateEventW(void*, BOOL, BOOL, const WCHAR*);
IMP BOOL SetEvent(HANDLE);
IMP BOOL ResetEvent(HANDLE);
IMP void InitializeCriticalSection(void*);
IMP void EnterCriticalSection(void*);
IMP void LeaveCriticalSection(void*);
IMP void DeleteCriticalSection(void*);
IMP void AcquireSRWLockExclusive(void*);
IMP void ReleaseSRWLockExclusive(void*);
IMP BOOL SleepConditionVariableSRW(void*, void*, DWORD, DWORD);
IMP void WakeAllConditionVariable(void*);
IMP DWORD TlsAlloc(void);
IMP BOOL TlsSetValue(DWORD, void*);
IMP void* TlsGetValue(DWORD);
IMP void* VirtualAlloc(void*, size_t, DWORD, DWORD);
IMP BOOL VirtualFree(void*, size_t, DWORD);
IMP HANDLE GetProcessHeap(void);
IMP void* HeapAlloc(HANDLE, DWORD, size_t);
IMP BOOL HeapFree(HANDLE, DWORD, void*);
IMP HANDLE GetModuleHandleA(const char*);
IMP void* GetProcAddress(HANDLE, const char*);
IMP HANDLE LoadLibraryA(const char*);
IMP DWORD GetModuleFileNameA(HANDLE, char*, DWORD);
IMP int MultiByteToWideChar(unsigned, DWORD, const char*, int, WCHAR*, int);
IMP HANDLE CreateFileA(const char*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
IMP BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
IMP BOOL SetFilePointerEx(HANDLE, int64_t, int64_t*, DWORD);
IMP BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
IMP BOOL GetFileSizeEx(HANDLE, int64_t*);
IMP DWORD GetFileAttributesA(const char*);
IMP HANDLE FindFirstFileA(const char*, void*);
IMP BOOL FindNextFileA(HANDLE, void*);
IMP BOOL FindClose(HANDLE);
IMP DWORD XbemDefinitelyMissing(void); /* deliberately not implemented by xbem */
IMP uint64_t XbemMissingWithArgs(uint64_t, uint64_t, uint64_t, uint64_t); /* ditto */

/* xgameruntime.dll */
typedef void (*XTaskQueueCallback)(void* context, _Bool canceled);
IMP HRESULT XGameRuntimeInitialize(void);
IMP uint32_t XSystemGetDeviceType(void);
IMP HRESULT XTaskQueueCreate(uint32_t work_mode, uint32_t completion_mode, void** queue);
IMP HRESULT XTaskQueueSubmitCallback(void* queue, uint32_t port, void* context, XTaskQueueCallback cb);
IMP _Bool XTaskQueueDispatch(void* queue, uint32_t port, uint32_t timeout_ms);
IMP void XTaskQueueCloseHandle(void* queue);
IMP HRESULT XUserAddAsync(uint32_t options, void* async_block);

/* guest_rt.c */
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void say(const char* text);
void say_num(const char* label, int64_t value);
void check_failed(int line, const char* expr);
int finish(const char* test_name);
size_t gstrlen(const char* s);
int gstreq(const char* a, const char* b);

#define CHECK(cond)                            \
  do {                                         \
    if (!(cond)) check_failed(__LINE__, #cond); \
  } while (0)

#define ATOMIC_INC(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define ATOMIC_LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
