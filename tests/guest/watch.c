/* Data watchpoints (run --watch): writes to a watched global and into a
   watched place of a VirtualAlloc block are logged; another write to the same
   page is stepped over silently; every value ends up as written. */
#include "guest.h"

__attribute__((dllexport)) volatile uint64_t watched_global = 1;

/* Exported, so the test can name the allocating function: alloc:watch.exe!alloc_block+OFFSET */
__attribute__((noinline, dllexport)) uint8_t* alloc_block(void) {
  uint8_t* p = (uint8_t*)VirtualAlloc(0, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  __asm__ volatile("" ::: "memory"); /* a real call (not a tail jump): the caller is alloc_block */
  return p;
}

DWORD guest_main(void* peb) {
  (void)peb;
  uint8_t* block = alloc_block();
  CHECK(block != 0);
  block[0x2100] = 5;                                          /* same page as the watch: silent */
  *(volatile uint64_t*)(block + 0x2008) = 0x1122334455667788ull; /* watched */
  watched_global = 2;
  watched_global = 3;
  CHECK(*(volatile uint64_t*)(block + 0x2008) == 0x1122334455667788ull);
  CHECK(block[0x2100] == 5 && watched_global == 3);
  return (DWORD)finish("watch");
}
