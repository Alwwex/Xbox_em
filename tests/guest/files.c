/* File system through the VFS: read-only package (G:), writable scratch (T:),
 * case-insensitive lookups, directory enumeration. */
#include "guest.h"

static int read_all(const char* path, char* buf, DWORD size, DWORD* got) {
  HANDLE f = CreateFileA(path, GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
  if (f == INVALID_HANDLE_VALUE) return 0;
  int ok = ReadFile(f, buf, size, got, 0);
  CloseHandle(f);
  return ok;
}

DWORD guest_main(void* peb) {
  (void)peb;
  char buf[64];
  DWORD got = 0;

  /* The file on disk is Content/Data.TXT - the guest uses different case. */
  CHECK(read_all("G:\\content\\data.txt", buf, sizeof buf - 1, &got));
  buf[got] = 0;
  CHECK(gstreq(buf, "hello-from-package"));
  CHECK(read_all("content/DATA.txt", buf, sizeof buf - 1, &got)); /* relative = package root */

  int64_t size = 0;
  HANDLE f = CreateFileA("G:\\Content\\Data.TXT", GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
  CHECK(f != INVALID_HANDLE_VALUE);
  CHECK(GetFileSizeEx(f, &size) && size == 18);
  CloseHandle(f);

  /* A synchronous handle keeps a file pointer: after a read at an OVERLAPPED
   * offset it points behind the bytes read (Windows semantics). */
  f = CreateFileA("G:\\Content\\Data.TXT", GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
  struct { uint64_t internal, internal_high; DWORD offset, offset_high; HANDLE event; } ov = {0, 0, 6, 0, 0};
  CHECK(ReadFile(f, buf, 4, &got, &ov) && got == 4 && buf[0] == 'f' && buf[3] == 'm'); /* "from" */
  CHECK(ReadFile(f, buf, 3, &got, 0) && got == 3 && buf[0] == '-' && buf[1] == 'p');    /* continues: "-pa" */
  int64_t pos = 0;
  CHECK(SetFilePointerEx(f, 0, &pos, 1) && pos == 13);
  CloseHandle(f);

  CHECK(GetFileAttributesA("G:\\Content") == 0x10);
  CHECK(GetFileAttributesA("G:\\nope.bin") == 0xFFFFFFFFu);
  CHECK(GetLastError() == 2);

  /* The package is read-only, like on the console. */
  HANDLE w = CreateFileA("G:\\new.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
  CHECK(w == INVALID_HANDLE_VALUE);
  CHECK(GetLastError() == 5);

  /* The scratch drive is writable. */
  w = CreateFileA("T:\\save.bin", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
  CHECK(w != INVALID_HANDLE_VALUE);
  DWORD written = 0;
  CHECK(WriteFile(w, "savegame", 8, &written, 0) && written == 8);
  CloseHandle(w);
  CHECK(read_all("T:\\SAVE.BIN", buf, sizeof buf - 1, &got) && got == 8);
  buf[got] = 0;
  CHECK(gstreq(buf, "savegame"));

  /* Enumeration (WIN32_FIND_DATAA is 320 bytes, name at offset 44). */
  unsigned char fd[320];
  HANDLE find = FindFirstFileA("G:\\content\\*.txt", fd);
  CHECK(find != INVALID_HANDLE_VALUE);
  int count = 0;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      say((const char*)fd + 44);
      ++count;
    } while (FindNextFileA(find, fd));
    FindClose(find);
  }
  CHECK(count == 2);

  return (DWORD)finish("files");
}
