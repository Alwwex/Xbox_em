#pragma once

#include <cstddef>
#include <cstdint>

namespace xbem::platform {

enum class Prot { None, R, RW, RX, RWX };

size_t page_size();

// Windows hands out address space in 64 KiB granules; guest code can depend on it.
constexpr size_t kAllocationGranularity = 0x10000;

// Maps `size` bytes at exactly `address` (no replacement of existing mappings).
// Returns nullptr if that range is not available.
void* map_fixed(uint64_t address, size_t size, Prot prot);

// Maps `size` bytes anywhere, with the start aligned to `alignment` (power of two).
void* map_anywhere(size_t size, size_t alignment, Prot prot);

bool unmap(void* address, size_t size);

// Copies `size` bytes from `address` without faulting if it is not mapped.
// Used to log the contents of guest pointers of unknown validity.
bool safe_read(uint64_t address, void* out, size_t size);
bool protect(void* address, size_t size, Prot prot);

// Drops the contents of committed pages and makes them inaccessible, keeping
// the address range reserved (Windows MEM_DECOMMIT).
bool decommit(void* address, size_t size);

// "Physical memory": a shared memory object whose pages can be mapped at one or
// more virtual addresses (console titles alias physical pages this way).
// Returns an opaque handle, or -1 on failure / unsupported host.
intptr_t create_shared_memory(uint64_t size);
// Maps [offset, offset+size) of `memory` at `address`, replacing whatever is there.
bool map_shared(uint64_t address, size_t size, Prot prot, intptr_t memory, uint64_t offset);
// Replaces [address, address+size) with fresh anonymous memory of protection `prot`.
bool remap_anonymous(uint64_t address, size_t size, Prot prot);
// Releases the host memory behind a range of the shared object (contents become zero).
bool discard_shared(intptr_t memory, uint64_t offset, size_t size);

inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

}  // namespace xbem::platform
