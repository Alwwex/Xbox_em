// kernelx: files, directories and console handles on top of the VFS
// (see runtime/vfs.h for how guest paths map to host paths).

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "hle/discovery.h"
#include "hle/file_log.h"
#include "hle/heap.h"
#include "hle/hle_util.h"
#include "platform/crash.h"
#include "platform/file_cache.h"

namespace xbem::hle {
namespace {

using namespace win;
namespace fs = std::filesystem;

constexpr DWORD GENERIC_READ = 0x80000000;
constexpr DWORD GENERIC_WRITE = 0x40000000;
constexpr DWORD GENERIC_ALL = 0x10000000;
constexpr DWORD FILE_WRITE_DATA = 0x2;
constexpr DWORD FILE_APPEND_DATA = 0x4;

constexpr DWORD CREATE_NEW = 1;
constexpr DWORD CREATE_ALWAYS = 2;
constexpr DWORD OPEN_EXISTING = 3;
constexpr DWORD OPEN_ALWAYS = 4;
constexpr DWORD TRUNCATE_EXISTING = 5;

constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x1;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF;

constexpr DWORD FILE_TYPE_UNKNOWN = 0;
constexpr DWORD FILE_TYPE_DISK = 1;
constexpr DWORD FILE_TYPE_CHAR = 2;

class FileObject final : public rt::KernelObject {
 public:
  ~FileObject() override {
    if (file) std::fclose(file);
  }
  const char* type_name() const override { return "File"; }
  std::mutex mutex;
  std::FILE* file = nullptr;
  fs::path path;
  std::string guest_path;
  bool writable = false;
  bool overlapped = false;  // FILE_FLAG_OVERLAPPED: the handle keeps no file pointer
  bool log_reads = false;   // --log-reads matched its name
  uint64_t position = 0;
};

class DirectoryObject final : public rt::KernelObject {
 public:
  const char* type_name() const override { return "Directory"; }
  fs::path path;
};

class ConsoleObject final : public rt::KernelObject {
 public:
  explicit ConsoleObject(int s) : stream(s) {}
  const char* type_name() const override { return "Console"; }
  int stream;  // 0 = in, 1 = out, 2 = err
};

class FindObject final : public rt::KernelObject {
 public:
  const char* type_name() const override { return "FindFile"; }
  std::vector<fs::directory_entry> entries;
  size_t next = 0;
  bool read_only = false;
};

std::FILE* open_host(const fs::path& p, const char* mode) {
#if defined(_WIN32)
  std::wstring wmode(mode, mode + std::strlen(mode));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return std::fopen(p.c_str(), mode);
#endif
}

bool seek_host(std::FILE* f, uint64_t pos) {
#if defined(_WIN32)
  return _fseeki64(f, static_cast<long long>(pos), SEEK_SET) == 0;
#else
  return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

uint64_t to_filetime(fs::file_time_type t) {
  using namespace std::chrono;
  const auto sys = clock_cast<system_clock>(t);
  const auto ns = duration_cast<nanoseconds>(sys.time_since_epoch()).count();
  return 116444736000000000ull + static_cast<uint64_t>(ns / 100);
}

// ------------------------------------------------------------- open/close

// --log-reads: file operations on files whose guest path contains the
// pattern (case-insensitive) are logged, the first 600 of them.
std::string g_read_log_pattern;
std::atomic<int> g_read_log_count{0}, g_dstorage_log_count{0};
constexpr int kReadLogLimit = 2000, kDstorageLogLimit = 100;

// Files the title opens and does not find (the first 64), with the host path
// they map to: a missing or misnamed file shows up here.
void log_missing_file(const std::string& guest_path, const fs::path& host_path) {
  static std::atomic<int> logged{0};
  const int n = logged.fetch_add(1);
  if (n < 64)
    XBEM_INFO("file", "not found: {} ({}){}", guest_path, host_path.string(),
              n == 63 ? " - further misses are not logged" : "");
}

HANDLE create_file(const std::string& guest_path, DWORD access, DWORD disposition, DWORD flags) {
  if (iequals(guest_path, "CONOUT$")) return proc().handles().insert(std::make_shared<ConsoleObject>(1));
  if (iequals(guest_path, "CONIN$")) return proc().handles().insert(std::make_shared<ConsoleObject>(0));

  auto r = proc().vfs().resolve(std::string_view(guest_path));
  if (!r) {
    XBEM_DEBUG("kernelx", "CreateFile({}): no mount for this path", guest_path);
    log_missing_file(guest_path, "(no drive mapping)");
    set_last_error(ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  const bool want_write = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
  std::error_code ec;
  const bool exists = fs::exists(r->host_path, ec);
  XBEM_TRACE("kernelx", "CreateFile({}) -> {} (exists={}, write={})", guest_path, r->host_path.string(), exists,
             want_write);

  if (exists && fs::is_directory(r->host_path, ec)) {
    if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
      set_last_error(ERROR_ACCESS_DENIED);
      return INVALID_HANDLE_VALUE;
    }
    auto d = std::make_shared<DirectoryObject>();
    d->path = r->host_path;
    return proc().handles().insert(d);
  }

  const bool creates = disposition == CREATE_NEW || disposition == CREATE_ALWAYS ||
                       disposition == TRUNCATE_EXISTING || (disposition == OPEN_ALWAYS && !exists);
  if (r->read_only && (want_write || creates)) {
    XBEM_DEBUG("kernelx", "CreateFile({}): write access to read-only package denied", guest_path);
    set_last_error(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }

  const char* mode = nullptr;
  DWORD success_error = ERROR_SUCCESS;
  switch (disposition) {
    case CREATE_NEW:
      if (exists) {
        set_last_error(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
      }
      mode = "w+b";
      break;
    case CREATE_ALWAYS:
      mode = "w+b";
      if (exists) success_error = ERROR_ALREADY_EXISTS;
      break;
    case OPEN_EXISTING:
      if (!exists) {
        set_last_error(fs::exists(r->host_path.parent_path(), ec) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
        log_missing_file(guest_path, r->host_path);
        return INVALID_HANDLE_VALUE;
      }
      mode = want_write ? "r+b" : "rb";
      break;
    case OPEN_ALWAYS:
      mode = exists ? (want_write ? "r+b" : "rb") : "w+b";
      if (exists) success_error = ERROR_ALREADY_EXISTS;
      break;
    case TRUNCATE_EXISTING:
      if (!exists) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
      }
      mode = "w+b";
      break;
    default:
      set_last_error(ERROR_INVALID_PARAMETER);
      return INVALID_HANDLE_VALUE;
  }

  std::FILE* f = open_host(r->host_path, mode);
  if (!f) {
    set_last_error(fs::exists(r->host_path.parent_path(), ec) ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  auto obj = std::make_shared<FileObject>();
  obj->file = f;
  obj->path = r->host_path;
  obj->guest_path = guest_path;
  obj->writable = mode[0] == 'w' || mode[1] == '+';
  obj->overlapped = (flags & 0x40000000u) != 0;  // FILE_FLAG_OVERLAPPED
  obj->log_reads = file_reads_logged(guest_path);
  if (obj->log_reads)
    XBEM_INFO("read", "CreateFile({}, access {:#x}, disposition {}, flags {:#x}) from {}", guest_path, access,
              disposition, flags, describe_code_address(XBEM_RETURN_ADDRESS()));
  set_last_error(success_error);
  return proc().handles().insert(obj);
}

HANDLE XBEM_GUEST_ABI hle_CreateFileW(LPCWSTR name, DWORD access, DWORD, void*, DWORD disposition, DWORD flags,
                                      HANDLE) {
  return create_file(narrow(name), access, disposition, flags);
}

HANDLE XBEM_GUEST_ABI hle_CreateFileA(LPCSTR name, DWORD access, DWORD, void*, DWORD disposition, DWORD flags,
                                      HANDLE) {
  return create_file(narrow(name), access, disposition, flags);
}

// CREATEFILE2_EXTENDED_PARAMETERS: { DWORD dwSize; DWORD dwFileAttributes; DWORD dwFileFlags; ... }
HANDLE XBEM_GUEST_ABI hle_CreateFile2(LPCWSTR name, DWORD access, DWORD, DWORD disposition, const DWORD* ext) {
  const DWORD flags = ext && ext[0] >= 12 ? ext[2] : 0;
  return create_file(narrow(name), access, disposition, flags);
}

// --------------------------------------------------------------- I/O

struct Overlapped {
  uint64_t internal;
  uint64_t internal_high;
  uint32_t offset;
  uint32_t offset_high;
  uint64_t event;
};

void complete_overlapped(Overlapped* ov, uint64_t bytes) {
  ov->internal = 0;  // STATUS_SUCCESS
  ov->internal_high = bytes;
  if (ov->event) {
    if (auto e = proc().handles().get_as<rt::EventObject>(ov->event)) e->set();
  }
}

BOOL XBEM_GUEST_ABI hle_ReadFile(HANDLE h, void* buffer, DWORD count, DWORD* read, Overlapped* ov) {
  auto obj = proc().handles().get(h);
  if (read) *read = 0;
  if (std::dynamic_pointer_cast<ConsoleObject>(obj)) return TRUE;  // no console input
  auto f = std::dynamic_pointer_cast<FileObject>(obj);
  if (!f) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  std::lock_guard lock(f->mutex);
  const uint64_t offset = ov ? (static_cast<uint64_t>(ov->offset_high) << 32 | ov->offset) : f->position;
  size_t got = 0;
  const bool watched = platform::overlaps_watched_page(reinterpret_cast<uint64_t>(buffer), count);
  if (!f->writable && !watched) {
    // Read-only (game) files: through the block cache (see platform/file_cache.h).
    const int64_t r = platform::cached_read(f->path.string(), offset, buffer, count);
    got = r > 0 ? static_cast<size_t>(r) : 0;
  } else if (seek_host(f->file, offset)) {
    if (watched) {
      // A watchpoint protects the destination: the kernel's write would fail
      // (EFAULT) instead of faulting, so copy in user space.
      std::vector<uint8_t> bounce(count);
      got = std::fread(bounce.data(), 1, count, f->file);
      std::memcpy(buffer, bounce.data(), got);
    } else {
      got = std::fread(buffer, 1, count, f->file);
    }
  }
  if (ov) complete_overlapped(ov, got);
  // A synchronous handle keeps its file pointer after the bytes read, also
  // when the read gave an offset in an OVERLAPPED (Windows semantics).
  if (!f->overlapped) f->position = offset + got;
  if (f->log_reads && take_read_log_slot())
  {
    const std::string dest = describe_destination(buffer);
    XBEM_INFO("read", "{} ReadFile at {:#x} size {:#x}{} -> {:#x} bytes into {}{} from {}", f->guest_path, offset,
              count, ov ? " (OVERLAPPED)" : "", got, buffer, dest.empty() ? "" : " (" + dest + ")",
              describe_code_address(XBEM_RETURN_ADDRESS()));
  }
  if (read) *read = static_cast<DWORD>(got);
  return TRUE;  // reading at EOF succeeds with 0 bytes
}

BOOL XBEM_GUEST_ABI hle_WriteFile(HANDLE h, const void* buffer, DWORD count, DWORD* written, Overlapped* ov) {
  auto obj = proc().handles().get(h);
  if (written) *written = 0;
  if (auto c = std::dynamic_pointer_cast<ConsoleObject>(obj)) {
    log::guest_output(std::string_view(static_cast<const char*>(buffer), count));
    if (written) *written = count;
    return TRUE;
  }
  auto f = std::dynamic_pointer_cast<FileObject>(obj);
  if (!f) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (!f->writable) {
    set_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  std::lock_guard lock(f->mutex);
  const uint64_t offset = ov ? (static_cast<uint64_t>(ov->offset_high) << 32 | ov->offset) : f->position;
  size_t put = 0;
  if (seek_host(f->file, offset)) put = std::fwrite(buffer, 1, count, f->file);
  if (ov) complete_overlapped(ov, put);
  if (!f->overlapped) f->position = offset + put;
  if (written) *written = static_cast<DWORD>(put);
  return put == count ? TRUE : FALSE;
}

BOOL XBEM_GUEST_ABI hle_GetOverlappedResult(HANDLE, Overlapped* ov, DWORD* transferred, BOOL) {
  if (!ov) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (transferred) *transferred = static_cast<DWORD>(ov->internal_high);
  return TRUE;  // all I/O completes synchronously
}

BOOL XBEM_GUEST_ABI hle_GetOverlappedResultEx(HANDLE h, Overlapped* ov, DWORD* transferred, DWORD, BOOL) {
  return hle_GetOverlappedResult(h, ov, transferred, TRUE);
}

std::shared_ptr<FileObject> file_from(HANDLE h) {
  auto f = proc().handles().get_as<FileObject>(h);
  if (!f) set_last_error(ERROR_INVALID_HANDLE);
  return f;
}

bool file_size(FileObject& f, uint64_t& size) {
  std::fflush(f.file);
  std::error_code ec;
  size = fs::file_size(f.path, ec);
  return !ec;
}

BOOL XBEM_GUEST_ABI hle_GetFileSizeEx(HANDLE h, LONGLONG* size) {
  auto f = file_from(h);
  uint64_t s = 0;
  if (!f || !size) return FALSE;
  std::lock_guard lock(f->mutex);
  if (!file_size(*f, s)) return FALSE;
  *size = static_cast<LONGLONG>(s);
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_GetFileSize(HANDLE h, DWORD* high) {
  auto f = file_from(h);
  uint64_t s = 0;
  if (!f) return 0xFFFFFFFF;
  std::lock_guard lock(f->mutex);
  if (!file_size(*f, s)) return 0xFFFFFFFF;
  if (high) *high = static_cast<DWORD>(s >> 32);
  return static_cast<DWORD>(s);
}

bool move_pointer(FileObject& f, int64_t distance, DWORD method, uint64_t& out) {
  int64_t base = 0;
  if (method == 1) base = static_cast<int64_t>(f.position);
  else if (method == 2) {
    uint64_t s = 0;
    if (!file_size(f, s)) return false;
    base = static_cast<int64_t>(s);
  } else if (method != 0) return false;
  if (base + distance < 0) return false;
  f.position = static_cast<uint64_t>(base + distance);
  out = f.position;
  return true;
}

BOOL XBEM_GUEST_ABI hle_SetFilePointerEx(HANDLE h, LONGLONG distance, LONGLONG* new_pos, DWORD method) {
  auto f = file_from(h);
  if (!f) return FALSE;
  std::lock_guard lock(f->mutex);
  uint64_t pos = 0;
  if (!move_pointer(*f, distance, method, pos)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (f->log_reads && take_read_log_slot())
    XBEM_INFO("read", "{} SetFilePointerEx({:#x}, method {}) -> {:#x} from {}", f->guest_path, distance, method, pos,
              describe_code_address(XBEM_RETURN_ADDRESS()));
  if (new_pos) *new_pos = static_cast<LONGLONG>(pos);
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_SetFilePointer(HANDLE h, LONG distance, LONG* high, DWORD method) {
  auto f = file_from(h);
  if (!f) return 0xFFFFFFFF;
  std::lock_guard lock(f->mutex);
  const int64_t d = high ? (static_cast<int64_t>(*high) << 32) | static_cast<uint32_t>(distance) : distance;
  uint64_t pos = 0;
  if (!move_pointer(*f, d, method, pos)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0xFFFFFFFF;
  }
  if (f->log_reads && take_read_log_slot())
    XBEM_INFO("read", "{} SetFilePointer({:#x}, method {}) -> {:#x} from {}", f->guest_path, d, method, pos,
              describe_code_address(XBEM_RETURN_ADDRESS()));
  if (high) *high = static_cast<LONG>(pos >> 32);
  set_last_error(ERROR_SUCCESS);
  return static_cast<DWORD>(pos);
}

BOOL XBEM_GUEST_ABI hle_FlushFileBuffers(HANDLE h) {
  auto f = file_from(h);
  if (!f) return FALSE;
  std::lock_guard lock(f->mutex);
  std::fflush(f->file);
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SetEndOfFile(HANDLE h) {
  auto f = file_from(h);
  if (!f) return FALSE;
  std::lock_guard lock(f->mutex);
  if (!f->writable) {
    set_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  std::fflush(f->file);
  std::error_code ec;
  fs::resize_file(f->path, f->position, ec);
  return ec ? FALSE : TRUE;
}

DWORD XBEM_GUEST_ABI hle_GetFileType(HANDLE h) {
  auto obj = proc().handles().get(h);
  if (std::dynamic_pointer_cast<ConsoleObject>(obj)) return FILE_TYPE_CHAR;
  if (std::dynamic_pointer_cast<FileObject>(obj) || std::dynamic_pointer_cast<DirectoryObject>(obj))
    return FILE_TYPE_DISK;
  set_last_error(ERROR_INVALID_HANDLE);
  return FILE_TYPE_UNKNOWN;
}

HANDLE XBEM_GUEST_ABI hle_GetStdHandle(DWORD which) {
  static HANDLE handles[3] = {};
  static std::once_flag once;
  std::call_once(once, [] {
    for (int i = 0; i < 3; ++i) handles[i] = proc().handles().insert(std::make_shared<ConsoleObject>(i));
  });
  switch (which) {
    case static_cast<DWORD>(-10): return handles[0];
    case static_cast<DWORD>(-11): return handles[1];
    case static_cast<DWORD>(-12): return handles[2];
    default:
      set_last_error(ERROR_INVALID_HANDLE);
      return INVALID_HANDLE_VALUE;
  }
}

// ----------------------------------------------------------- attributes

DWORD attributes_of(const fs::path& p, bool read_only_mount) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) return FILE_ATTRIBUTE_DIRECTORY;
  return read_only_mount ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
}

DWORD get_attributes(const std::string& guest_path) {
  auto r = proc().vfs().resolve(std::string_view(guest_path));
  std::error_code ec;
  if (!r || !fs::exists(r->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
  }
  return attributes_of(r->host_path, r->read_only);
}

DWORD XBEM_GUEST_ABI hle_GetFileAttributesW(LPCWSTR name) { return get_attributes(narrow(name)); }
DWORD XBEM_GUEST_ABI hle_GetFileAttributesA(LPCSTR name) { return get_attributes(narrow(name)); }

void fill_times_and_size(uint8_t* out, const fs::path& p) {
  std::error_code ec;
  const uint64_t ft = to_filetime(fs::last_write_time(p, ec));
  const uint64_t size = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0;
  const DWORD hi = static_cast<DWORD>(size >> 32), lo = static_cast<DWORD>(size);
  std::memcpy(out + 0, &ft, 8);   // creation
  std::memcpy(out + 8, &ft, 8);   // last access
  std::memcpy(out + 16, &ft, 8);  // last write
  std::memcpy(out + 24, &hi, 4);
  std::memcpy(out + 28, &lo, 4);
}

BOOL XBEM_GUEST_ABI hle_GetFileAttributesExW(LPCWSTR name, DWORD level, uint8_t* data) {
  if (level != 0 || !data) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  auto r = proc().vfs().resolve(guest_wstr(name));
  std::error_code ec;
  if (!r || !fs::exists(r->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  // WIN32_FILE_ATTRIBUTE_DATA: DWORD attrs; FILETIME x3; DWORD size_high; DWORD size_low
  const DWORD attrs = attributes_of(r->host_path, r->read_only);
  std::memcpy(data, &attrs, 4);
  fill_times_and_size(data + 4, r->host_path);
  return TRUE;
}

std::optional<rt::Vfs::Resolved> resolve_writable(const std::string& guest_path) {
  auto r = proc().vfs().resolve(std::string_view(guest_path));
  if (!r) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return std::nullopt;
  }
  if (r->read_only) {
    set_last_error(ERROR_ACCESS_DENIED);
    return std::nullopt;
  }
  return r;
}

BOOL XBEM_GUEST_ABI hle_DeleteFileW(LPCWSTR name) {
  auto r = resolve_writable(narrow(name));
  if (!r) return FALSE;
  std::error_code ec;
  if (!fs::is_regular_file(r->host_path, ec) || !fs::remove(r->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_CreateDirectoryW(LPCWSTR name, void*) {
  auto r = resolve_writable(narrow(name));
  if (!r) return FALSE;
  std::error_code ec;
  if (fs::exists(r->host_path, ec)) {
    set_last_error(ERROR_ALREADY_EXISTS);
    return FALSE;
  }
  if (!fs::create_directory(r->host_path, ec)) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_CreateDirectoryA(LPCSTR name, void* sa) {
  const std::u16string w = utf8_to_utf16(guest_cstr(name));
  return hle_CreateDirectoryW(w.c_str(), sa);
}

BOOL XBEM_GUEST_ABI hle_RemoveDirectoryW(LPCWSTR name) {
  auto r = resolve_writable(narrow(name));
  if (!r) return FALSE;
  std::error_code ec;
  if (!fs::is_directory(r->host_path, ec) || !fs::remove(r->host_path, ec)) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return FALSE;
  }
  return TRUE;
}

// ------------------------------------------------------------ find files

template <class Char>
void fill_find_data(uint8_t* out, const fs::directory_entry& e, bool read_only) {
  const size_t total = sizeof(Char) == 2 ? 592 : 320;  // WIN32_FIND_DATAW / WIN32_FIND_DATAA
  std::memset(out, 0, total);
  const DWORD attrs = attributes_of(e.path(), read_only);
  std::memcpy(out, &attrs, 4);
  fill_times_and_size(out + 4, e.path());
  const std::string name = e.path().filename().string();
  auto* fname = reinterpret_cast<Char*>(out + 44);
  if constexpr (sizeof(Char) == 2) {
    const std::u16string w = utf8_to_utf16(name);
    const size_t n = std::min<size_t>(w.size(), 259);
    std::memcpy(fname, w.data(), n * 2);
  } else {
    std::memcpy(fname, name.data(), std::min<size_t>(name.size(), 259));
  }
}

template <class Char>
HANDLE find_first(const std::string& pattern_path, uint8_t* data) {
  const size_t cut = pattern_path.find_last_of("\\/");
  const std::string dir = cut == std::string::npos ? std::string() : pattern_path.substr(0, cut + 1);
  const std::string pattern = cut == std::string::npos ? pattern_path : pattern_path.substr(cut + 1);
  auto r = proc().vfs().resolve(std::string_view(dir.empty() ? std::string(".") : dir));
  std::error_code ec;
  if (!r || !fs::is_directory(r->host_path, ec)) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  auto find = std::make_shared<FindObject>();
  find->read_only = r->read_only;
  for (fs::directory_iterator it(r->host_path, ec), end; !ec && it != end; it.increment(ec))
    if (wildcard_match_icase(pattern, it->path().filename().string())) find->entries.push_back(*it);
  if (find->entries.empty()) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  fill_find_data<Char>(data, find->entries[0], find->read_only);
  find->next = 1;
  return proc().handles().insert(find);
}

template <class Char>
BOOL find_next(HANDLE h, uint8_t* data) {
  auto find = proc().handles().get_as<FindObject>(h);
  if (!find) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (find->next >= find->entries.size()) {
    set_last_error(ERROR_NO_MORE_FILES);
    return FALSE;
  }
  fill_find_data<Char>(data, find->entries[find->next++], find->read_only);
  return TRUE;
}

HANDLE XBEM_GUEST_ABI hle_FindFirstFileW(LPCWSTR name, uint8_t* data) { return find_first<char16_t>(narrow(name), data); }
HANDLE XBEM_GUEST_ABI hle_FindFirstFileA(LPCSTR name, uint8_t* data) { return find_first<char>(narrow(name), data); }
HANDLE XBEM_GUEST_ABI hle_FindFirstFileExW(LPCWSTR name, DWORD, uint8_t* data, DWORD, void*, DWORD) {
  return find_first<char16_t>(narrow(name), data);
}
BOOL XBEM_GUEST_ABI hle_FindNextFileW(HANDLE h, uint8_t* data) { return find_next<char16_t>(h, data); }
BOOL XBEM_GUEST_ABI hle_FindNextFileA(HANDLE h, uint8_t* data) { return find_next<char>(h, data); }

BOOL XBEM_GUEST_ABI hle_FindClose(HANDLE h) {
  if (!proc().handles().get_as<FindObject>(h)) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  proc().handles().close(h);
  return TRUE;
}

// ---------------------------------------------------- directories/paths

std::u16string drive_root(char drive) { return std::u16string{static_cast<char16_t>(drive), u':', u'\\'}; }

DWORD XBEM_GUEST_ABI hle_GetCurrentDirectoryW(DWORD size, LPWSTR buf) {
  return copy_out_string<char16_t>(drive_root(proc().vfs().default_drive()), buf, size);
}

BOOL XBEM_GUEST_ABI hle_SetCurrentDirectoryW(LPCWSTR path) {
  XBEM_WARN_ONCE("kernelx", "SetCurrentDirectory({}) ignored: the working directory is the package root",
                 narrow(path));
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_GetTempPathW(DWORD size, LPWSTR buf) {
  return copy_out_string<char16_t>(drive_root('T'), buf, size);
}

DWORD XBEM_GUEST_ABI hle_GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buf, LPWSTR* file_part) {
  std::u16string p(guest_wstr(name));
  for (auto& c : p)
    if (c == u'/') c = u'\\';
  const bool absolute = p.size() >= 2 && p[1] == u':';
  if (!absolute) {
    const std::u16string root = drive_root(proc().vfs().default_drive());
    p = (!p.empty() && p[0] == u'\\') ? root.substr(0, 2) + p : root + p;
  }
  const DWORD n = copy_out_string<char16_t>(p, buf, size);
  if (file_part && buf && n < size) {
    const size_t cut = p.find_last_of(u'\\');
    *file_part = (cut == std::u16string::npos || cut + 1 >= p.size()) ? nullptr : buf + cut + 1;
  }
  return n;
}

// ------------------------------------------------- handle-based metadata

std::optional<fs::path> path_of_handle(HANDLE h) {
  auto obj = lookup_handle(h);
  if (auto f = std::dynamic_pointer_cast<FileObject>(obj)) return f->path;
  if (auto d = std::dynamic_pointer_cast<DirectoryObject>(obj)) return d->path;
  return std::nullopt;
}

uint64_t from_filetime_to_host(uint64_t ft, fs::file_time_type& out) {
  using namespace std::chrono;
  const auto ns = nanoseconds(static_cast<int64_t>(ft - 116444736000000000ull) * 100);
  out = clock_cast<fs::file_time_type::clock>(system_clock::time_point(duration_cast<system_clock::duration>(ns)));
  return ft;
}

BOOL XBEM_GUEST_ABI hle_GetFileTime(HANDLE h, uint64_t* creation, uint64_t* access, uint64_t* write) {
  auto p = path_of_handle(h);
  if (!p) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  std::error_code ec;
  const uint64_t ft = to_filetime(fs::last_write_time(*p, ec));
  if (creation) *creation = ft;
  if (access) *access = ft;
  if (write) *write = ft;
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SetFileTime(HANDLE h, const uint64_t*, const uint64_t*, const uint64_t* write) {
  auto p = path_of_handle(h);
  if (!p) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (write && *write) {
    fs::file_time_type t;
    from_filetime_to_host(*write, t);
    std::error_code ec;
    fs::last_write_time(*p, t, ec);  // best effort (fails silently on the read-only package)
  }
  return TRUE;
}

// BY_HANDLE_FILE_INFORMATION (52 bytes)
BOOL XBEM_GUEST_ABI hle_GetFileInformationByHandle(HANDLE h, uint8_t* info) {
  auto p = path_of_handle(h);
  if (!p || !info) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  std::memset(info, 0, 52);
  const DWORD attrs = attributes_of(*p, false);
  std::memcpy(info, &attrs, 4);
  uint8_t times[32];
  fill_times_and_size(times, *p);
  std::memcpy(info + 4, times, 24);       // creation, access, write
  const DWORD serial = 0x58424D45;        // "XBME"
  std::memcpy(info + 28, &serial, 4);
  std::memcpy(info + 32, times + 24, 8);  // size high, size low
  const DWORD links = 1;
  std::memcpy(info + 40, &links, 4);
  const uint64_t index = std::hash<std::string>{}(p->string());
  const DWORD hi = static_cast<DWORD>(index >> 32), lo = static_cast<DWORD>(index);
  std::memcpy(info + 44, &hi, 4);
  std::memcpy(info + 48, &lo, 4);
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SetFileAttributesW(LPCWSTR name, DWORD) {
  auto r = resolve_writable(narrow(name));
  if (!r) return FALSE;
  std::error_code ec;
  if (!fs::exists(r->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  return TRUE;  // attributes beyond read-only/directory are not modelled
}

BOOL XBEM_GUEST_ABI hle_GetDiskFreeSpaceExW(LPCWSTR dir, uint64_t* avail, uint64_t* total, uint64_t* total_free) {
  const std::string path = dir ? narrow(dir) : std::string();
  auto r = proc().vfs().resolve(std::string_view(path.empty() ? std::string("T:\\") : path));
  uint64_t a = 0, t = 0, f = 0;
  if (r && !r->read_only) {
    std::error_code ec;
    const fs::space_info si = fs::space(r->host_path, ec);
    if (!ec) {
      a = si.available;
      t = si.capacity;
      f = si.free;
    }
  } else if (r) {
    std::error_code ec;
    const fs::space_info si = fs::space(r->host_path, ec);
    t = ec ? 0 : si.capacity;  // the package drive is full and read-only
  } else {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return FALSE;
  }
  if (avail) *avail = a;
  if (total) *total = t;
  if (total_free) *total_free = f;
  return TRUE;
}

constexpr DWORD MOVEFILE_REPLACE_EXISTING = 0x1;
constexpr DWORD MOVEFILE_COPY_ALLOWED = 0x2;

BOOL XBEM_GUEST_ABI hle_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  auto src = resolve_writable(narrow(from));
  if (!src) return FALSE;
  if (!to) {  // delete (normally only after reboot)
    std::error_code ec;
    fs::remove(src->host_path, ec);
    return TRUE;
  }
  auto dst = resolve_writable(narrow(to));
  if (!dst) return FALSE;
  std::error_code ec;
  if (!fs::exists(src->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  if (fs::exists(dst->host_path, ec) && !(flags & MOVEFILE_REPLACE_EXISTING)) {
    set_last_error(ERROR_ALREADY_EXISTS);
    return FALSE;
  }
  fs::rename(src->host_path, dst->host_path, ec);
  if (ec && (flags & MOVEFILE_COPY_ALLOWED)) {
    ec.clear();
    fs::copy(src->host_path, dst->host_path, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
    if (!ec) fs::remove_all(src->host_path, ec);
  }
  if (ec) {
    set_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return TRUE;
}

constexpr HRESULT hresult_from_win32(DWORD e) { return e ? static_cast<HRESULT>(0x80070000u | (e & 0xFFFF)) : S_OK; }

// HRESULT CopyFile2(PCWSTR existing, PCWSTR new, COPYFILE2_EXTENDED_PARAMETERS*)
HRESULT XBEM_GUEST_ABI hle_CopyFile2(LPCWSTR from, LPCWSTR to, const uint8_t* ext) {
  DWORD copy_flags = 0;
  if (ext) std::memcpy(&copy_flags, ext + 4, 4);
  auto src = proc().vfs().resolve(guest_wstr(from));
  auto dst = resolve_writable(narrow(to));
  if (!src) return hresult_from_win32(ERROR_PATH_NOT_FOUND);
  if (!dst) return hresult_from_win32(last_error());
  std::error_code ec;
  if (!fs::is_regular_file(src->host_path, ec)) return hresult_from_win32(ERROR_FILE_NOT_FOUND);
  if ((copy_flags & 0x1 /*COPY_FILE_FAIL_IF_EXISTS*/) && fs::exists(dst->host_path, ec))
    return hresult_from_win32(ERROR_FILE_EXISTS);
  fs::copy_file(src->host_path, dst->host_path, fs::copy_options::overwrite_existing, ec);
  return ec ? hresult_from_win32(ERROR_ACCESS_DENIED) : S_OK;
}

BOOL XBEM_GUEST_ABI hle_CreateHardLinkW(LPCWSTR link, LPCWSTR existing, void*) {
  auto l = resolve_writable(narrow(link));
  auto e = proc().vfs().resolve(guest_wstr(existing));
  if (!l || !e) {
    if (!e) set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  std::error_code ec;
  fs::create_hard_link(e->host_path, l->host_path, ec);
  if (ec) fs::copy_file(e->host_path, l->host_path, ec);  // across mounts: fall back to a copy
  if (ec) {
    set_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return TRUE;
}

BOOLEAN XBEM_GUEST_ABI hle_CreateSymbolicLinkW(LPCWSTR link, LPCWSTR target, DWORD flags) {
  auto l = resolve_writable(narrow(link));
  auto t = proc().vfs().resolve(guest_wstr(target));
  if (!l || !t) {
    if (!t) set_last_error(ERROR_PATH_NOT_FOUND);
    return 0;
  }
  std::error_code ec;
  if (flags & 0x1 /*SYMBOLIC_LINK_FLAG_DIRECTORY*/) fs::create_directory_symlink(t->host_path, l->host_path, ec);
  else fs::create_symlink(t->host_path, l->host_path, ec);
  if (ec) {
    set_last_error(ERROR_ACCESS_DENIED);
    return 0;
  }
  return 1;
}

// Named pipes do not exist for titles here.
BOOL XBEM_GUEST_ABI hle_SetNamedPipeHandleState(HANDLE, DWORD*, DWORD*, DWORD*) {
  set_last_error(ERROR_INVALID_HANDLE);
  return FALSE;
}
BOOL XBEM_GUEST_ABI hle_WaitNamedPipeW(LPCWSTR, DWORD) {
  set_last_error(ERROR_FILE_NOT_FOUND);
  return FALSE;
}

}  // namespace

void add_kernelx_file_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(CreateFileW), XBEM_HLE(CreateFileA), XBEM_HLE(CreateFile2), XBEM_HLE(ReadFile), XBEM_HLE(WriteFile),
      XBEM_HLE(GetOverlappedResult), XBEM_HLE(GetOverlappedResultEx), XBEM_HLE(GetFileSizeEx), XBEM_HLE(GetFileSize),
      XBEM_HLE(SetFilePointerEx), XBEM_HLE(SetFilePointer), XBEM_HLE(FlushFileBuffers), XBEM_HLE(SetEndOfFile),
      XBEM_HLE(GetFileType), XBEM_HLE(GetStdHandle), XBEM_HLE(GetFileAttributesW), XBEM_HLE(GetFileAttributesA),
      XBEM_HLE(GetFileAttributesExW), XBEM_HLE(DeleteFileW), XBEM_HLE(CreateDirectoryW), XBEM_HLE(CreateDirectoryA),
      XBEM_HLE(RemoveDirectoryW), XBEM_HLE(FindFirstFileW), XBEM_HLE(FindFirstFileA), XBEM_HLE(FindFirstFileExW),
      XBEM_HLE(FindNextFileW), XBEM_HLE(FindNextFileA), XBEM_HLE(FindClose), XBEM_HLE(GetCurrentDirectoryW),
      XBEM_HLE(SetCurrentDirectoryW), XBEM_HLE(GetTempPathW), XBEM_HLE(GetFullPathNameW),
      XBEM_HLE(GetFileTime), XBEM_HLE(SetFileTime), XBEM_HLE(GetFileInformationByHandle),
      XBEM_HLE(SetFileAttributesW), XBEM_HLE(GetDiskFreeSpaceExW), XBEM_HLE(MoveFileExW), XBEM_HLE(CopyFile2),
      XBEM_HLE(CreateHardLinkW), XBEM_HLE(CreateSymbolicLinkW), XBEM_HLE_STUB(SetNamedPipeHandleState),
      XBEM_HLE_STUB(WaitNamedPipeW),
  });
}

void set_file_read_log(const std::string& pattern) { g_read_log_pattern = pattern; }

bool file_reads_logged(const std::string& guest_path) {
  if (g_read_log_pattern.empty()) return false;
  std::string a = guest_path, b = g_read_log_pattern;
  for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return a.find(b) != std::string::npos;
}

bool take_read_log_slot() {
  const int n = g_read_log_count.fetch_add(1);
  if (n == kReadLogLimit) XBEM_INFO("read", "(further file operations are not logged)");
  return n < kReadLogLimit;
}

bool take_dstorage_log_slot() {
  const int n = g_dstorage_log_count.fetch_add(1);
  if (n == kDstorageLogLimit) XBEM_INFO("read", "(further DirectStorage requests are not logged)");
  return n < kDstorageLogLimit;
}

std::string describe_destination(const void* address) {
  char where[256] = {};
  if (describe_virtual_memory(reinterpret_cast<uint64_t>(address), where, sizeof where)) return where;
  if (describe_xmem_allocation(reinterpret_cast<uint64_t>(address), where, sizeof where)) return where;
  return {};
}

}  // namespace xbem::hle
