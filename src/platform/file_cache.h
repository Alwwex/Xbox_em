#pragma once

// Read cache for the title's (read-only) game files.
//
// Game packages often live on a slow file system - on WSL2 a Windows drive
// under /mnt is reached through 9P, where every read is a round trip. Titles
// stream thousands of small reads (DirectStorage requests, ReadFile on .rpf
// archives); GTA VI's story load issued them faster than 9P answered and its
// loading watchdog fired. Reads go through 256 KiB blocks kept in an LRU cache
// (XBEM_READ_CACHE_MB, default 512; 0 disables it).

#include <cstddef>
#include <cstdint>
#include <string>

namespace xbem::platform {

// Reads up to `n` bytes at `offset` of the file at `host_path` into `out`;
// returns the number of bytes read (short at the end of the file), or -1 when
// the file cannot be opened.
int64_t cached_read(const std::string& host_path, uint64_t offset, void* out, size_t n);

}  // namespace xbem::platform
