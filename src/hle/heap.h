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
size_t virtual_query(const void* address, uint8_t* mbi, size_t length);  // fills a 48-byte MEMORY_BASIC_INFORMATION

// run --watch alloc:CALLER+OFFSET: a data watchpoint armed at `offset` into
// the first committed allocation made from code in [caller_begin, caller_end)
// (the function holding the allocating call) that is larger than `offset`.
void add_allocation_watch(uint64_t caller_begin, uint64_t caller_end, uint64_t offset, uint32_t length,
                          const std::string& label);

}  // namespace xbem::hle
