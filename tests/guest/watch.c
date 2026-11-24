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

/* For --watch at:watch.exe!store_at?rdx=2:rcx+0x3000: armed on the second
   call, at an address only known then. Its first instruction writes the
   watched place itself, so the tracepoint's single step is also the
   watchpoint's. */
__attribute__((naked, noinline, dllexport)) void store_at(uint8_t* p, uint64_t value) {
  __asm__ volatile("movq %rdx, 0x3000(%rcx)\n\tret");
}

/* A tracepoint reached after the watched step (--trace-at watch.exe!after_store). */
__attribute__((noinline, dllexport)) uint64_t after_store(uint8_t* p) {
  __asm__ volatile("" ::: "memory");
  return *(volatile uint64_t*)(p + 0x3000);
}

DWORD guest_main(void* peb) {
  (void)peb;
  uint8_t* block = alloc_block();
  CHECK(block != 0);
  store_at(block, 1); /* before the watchpoint is armed: not logged */
  store_at(block, 2); /* arms it; the write itself is the first logged one */
  store_at(block, 3);
  CHECK(after_store(block) == 3);
  block[0x2100] = 5;                                          /* same page as the watch: silent */
  *(volatile uint64_t*)(block + 0x2008) = 0x1122334455667788ull; /* watched */
  watched_global = 2;
  watched_global = 3;
  CHECK(*(volatile uint64_t*)(block + 0x2008) == 0x1122334455667788ull);
  CHECK(block[0x2100] == 5 && watched_global == 3);
  return (DWORD)finish("watch");
}
