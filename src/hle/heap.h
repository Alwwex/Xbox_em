#pragma once

// Block allocator shared by the kernel heap API (HeapAlloc, LocalAlloc, ...)
// and the C runtime (malloc, operator new via the CRT). Blocks are 16-byte
// aligned and remember their size.

#include <cstddef>
#include <cstdint>
#include <string>

namespace xbem::hle {

void* heap_alloc(size_t size, bool zero);
bool heap_free(void* p);  // false if `p` was not allocated here
void* heap_realloc(void* p, size_t size, bool zero_new);
bool heap_owns(const void* p);
size_t heap_block_size(const void* p);  // ~0 if not owned

// Virtual memory (VirtualAlloc/VirtualFree/VirtualQuery semantics, Win32 last
// error set on failure). `alignment` 0 = default for the allocation type.
void* virtual_alloc(void* address, uint64_t size, uint32_t type, uint32_t protect, uint64_t alignment,
                    uint64_t caller = 0);
bool virtual_free(void* address, uint64_t size, uint32_t type);
// For crash reports: describes the VirtualAlloc region containing `address`
// (who allocated it and with which protection). Never blocks.
bool describe_virtual_memory(uint64_t address, char* out, size_t out_size);
// The same for blocks from XMemAlloc / XMemAllocDefault.
bool describe_xmem_allocation(uint64_t address, char* out, size_t out_size);
// Writes into the emulated physical page with frame number `pfn` (4 KiB
// units, as XMemAllocatePhysicalPages hands them out) at `offset` bytes from
// the start of its 64 KiB page. False for pages that are not allocated.
bool write_physical_page(uint64_t pfn, uint64_t offset, const void* data, size_t size);
// Remembers which DirectStorage request last wrote the page of `pfn` (`how`:
// 0 inflated, 1 copied, 2 from a decoded chunk, 3 zeros); crash reports
// name it. `file` must stay valid.
void note_physical_write(uint64_t pfn, const char* file, uint64_t offset, uint64_t virt, uint32_t how);
// Emulated physical memory: which physical page `address` maps and where else
// that page is mapped (never blocks); and every address the byte at `address`
// is mapped at (a platform::ViewFinder for watchpoints).
bool describe_physical_page(uint64_t address, char* out, size_t out_size);
int physical_views(uint64_t address, uint64_t* out, int max, bool may_block);
size_t virtual_query(const void* address, uint8_t* mbi, size_t length);  // fills a 48-byte MEMORY_BASIC_INFORMATION

// run --watch alloc:CALLER+OFFSET: a data watchpoint armed at `offset` into
// the first committed allocation made from code in [caller_begin, caller_end)
// (the function holding the allocating call) that is larger than `offset`.
void add_allocation_watch(uint64_t caller_begin, uint64_t caller_end, uint64_t offset, uint32_t length,
                          const std::string& label);

}  // namespace xbem::hle
