#pragma once

// Internals shared by the C runtime (UCRT) HLE files: errno, guest FILE
// objects, the printf/scanf engines and guest va_list access.
//
// The console ships the C runtime as part of the OS (ucrtbase.dll behind the
// api-ms-win-crt-* API sets), so titles import it but do not bundle it. The
// msvcrt.dll name is served too, which lets MinGW-built test programs use it.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hle/hle_util.h"

#if defined(_WIN32)
#include <cstdarg>
#endif

namespace xbem::hle::crt {

// errno values (identical to the MSVC CRT for everything used here).
constexpr int kEPERM = 1, kENOENT = 2, kEIO = 5, kEBADF = 9, kENOMEM = 12, kEACCES = 13, kEEXIST = 17,
              kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kEMFILE = 24, kENOSPC = 28, kESPIPE = 29, kERANGE = 34,
              kEILSEQ = 42, kSTRUNCATE = 80;

int* errno_ptr();
inline void set_errno(int e) { *errno_ptr() = e; }

// Called when the CRT detects an invalid argument (what _invalid_parameter
// does on Windows). Logs once per call site and returns.
void invalid_parameter(const char* function);

// ---------------------------------------------------------------- va_list
// A Microsoft x64 va_list is a plain pointer to consecutive 8-byte slots.
#if defined(_WIN32)
using GuestVaList = va_list;
#define XBEM_GUEST_VA_START(ap, last) va_start(ap, last)
#define XBEM_GUEST_VA_END(ap) va_end(ap)
#else
using GuestVaList = __builtin_ms_va_list;
#define XBEM_GUEST_VA_START(ap, last) __builtin_ms_va_start(ap, last)
#define XBEM_GUEST_VA_END(ap) __builtin_ms_va_end(ap)
#endif

struct ArgReader {
  const uint8_t* cursor;
  uint64_t next_u64() {
    uint64_t v;
    std::memcpy(&v, cursor, 8);
    cursor += 8;
    return v;
  }
  double next_double() {
    double v;
    std::memcpy(&v, cursor, 8);
    cursor += 8;
    return v;
  }
  template <class T>
  T* next_ptr() {
    return reinterpret_cast<T*>(next_u64());
  }
};

inline ArgReader args_of(GuestVaList ap) { return ArgReader{reinterpret_cast<const uint8_t*>(ap)}; }
inline ArgReader args_of(const void* ap) { return ArgReader{static_cast<const uint8_t*>(ap)}; }

// ------------------------------------------------------------- formatting
// UCRT __stdio_common_* option bits.
constexpr uint64_t kPrintfLegacyVsprintfNullTermination = 0x1;
constexpr uint64_t kPrintfStandardSnprintfBehavior = 0x2;
constexpr uint64_t kPrintfLegacyWideSpecifiers = 0x4;

std::string format_narrow(const char* format, ArgReader& args, uint64_t options);
std::u16string format_wide(const char16_t* format, ArgReader& args, uint64_t options);

int scan_narrow(const char* input, const char* format, ArgReader& args);
int scan_wide(const char16_t* input, const char16_t* format, ArgReader& args);

// ------------------------------------------------------------ guest FILE
// Guest FILE* values point at these. The first three live in an array laid
// out like msvcrt's _iob (48-byte entries) so &__iob_func()[2] == stderr.
struct GuestFile {
  uint64_t magic;
  std::FILE* host;
  int32_t fd;     // guest file descriptor, -1 if none
  int32_t flags;  // kFile* bits
  // Exposed through _get_stream_buffer_pointers. They always describe an
  // empty buffer, so the MSVC STL falls back to fgetc/fputc & co.
  char* buffer_base;
  char* buffer_ptr;
  int32_t buffer_count;
  int32_t pad;
};
constexpr int32_t kFileText = 0x1;      // CRLF -> LF translation on read
constexpr int32_t kFileReadOnly = 0x2;
constexpr int32_t kFileStd = 0x4;       // stdin/stdout/stderr, never closed
static_assert(sizeof(GuestFile) == 48);

constexpr uint64_t kGuestFileMagic = 0x454C4946'4D454258ull;  // "XBEMFILE"

GuestFile* guest_file(void* f);       // nullptr (and EBADF) if not a guest FILE
GuestFile* std_stream(unsigned index);  // 0 stdin, 1 stdout, 2 stderr
GuestFile* open_guest_file(const std::string& guest_path, const std::string& mode, int* error);
int close_guest_file(GuestFile* f);
size_t write_guest_file(GuestFile* f, const void* data, size_t size);
GuestFile* file_from_fd(int fd);

// Environment shared with kernelx (Get/SetEnvironmentVariable).
std::optional<std::u16string> env_get(const std::string& name);
void env_set(const std::string& name, const std::optional<std::u16string>& value);

// Wide/narrow conversion used throughout the CRT ("C" locale + UTF-8).
std::string to_narrow(std::u16string_view w);
std::u16string to_wide(std::string_view s);

}  // namespace xbem::hle::crt

namespace xbem::hle {
ModuleDef make_ucrt_module();
void add_ucrt_runtime_exports(std::vector<ExportDef>& out);
void add_ucrt_string_exports(std::vector<ExportDef>& out);
void add_ucrt_stdio_exports(std::vector<ExportDef>& out);
void add_ucrt_math_exports(std::vector<ExportDef>& out);
void add_ucrt_misc_exports(std::vector<ExportDef>& out);
}  // namespace xbem::hle
