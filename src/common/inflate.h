#pragma once

// DEFLATE (RFC 1951) decompression, with or without the zlib wrapper
// (RFC 1950). Used for the console's DirectStorage requests, whose
// compressed chunks the hardware decompressor would otherwise inflate.

#include <cstddef>
#include <cstdint>

namespace xbem {

enum class InflateResult { Ok, OutputFull, BadData, Truncated };

// Raw DEFLATE stream. `out_size` receives the bytes written.
InflateResult inflate_raw(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t& out_size);

// zlib stream (2-byte header, DEFLATE data, Adler-32 not checked); falls back
// to raw DEFLATE when the header is not a zlib header.
InflateResult inflate_auto(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t& out_size);

const char* inflate_result_name(InflateResult r);

}  // namespace xbem
