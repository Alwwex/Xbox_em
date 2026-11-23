#pragma once

// Block allocator shared by the kernel heap API (HeapAlloc, LocalAlloc, ...)
// and the C runtime (malloc, operator new via the CRT). Blocks are 16-byte
// aligned and remember their size.

#include <cstddef>

namespace xbem::hle {

void* heap_alloc(size_t size, bool zero);
bool heap_free(void* p);  // false if `p` was not allocated here
void* heap_realloc(void* p, size_t size, bool zero_new);
bool heap_owns(const void* p);
size_t heap_block_size(const void* p);  // ~0 if not owned

}  // namespace xbem::hle
