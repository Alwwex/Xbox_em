#include "common/inflate.h"

#include <cstring>

namespace xbem {
namespace {

// Canonical Huffman decoding by code length counts (as in zlib's "puff").
struct Huffman {
  uint16_t count[16];  // number of codes of each length
  uint16_t symbol[288];
};

struct State {
  const uint8_t* in;
  size_t in_size, in_pos = 0;
  uint32_t bit_buf = 0;
  int bit_count = 0;
  uint8_t* out = nullptr;
  size_t out_capacity = 0, out_pos = 0;
  bool truncated = false;
};

int bits(State& s, int need) {
  uint32_t val = s.bit_buf;
  while (s.bit_count < need) {
    if (s.in_pos >= s.in_size) {
      s.truncated = true;
      return 0;
    }
    val |= static_cast<uint32_t>(s.in[s.in_pos++]) << s.bit_count;
    s.bit_count += 8;
  }
  s.bit_buf = val >> need;
  s.bit_count -= need;
  return static_cast<int>(val & ((1u << need) - 1));
}

int decode(State& s, const Huffman& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; ++len) {
    code |= bits(s, 1);
    if (s.truncated) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -2;  // ran out of codes
}

// Returns 0 for a complete code, >0 for an incomplete one (allowed for
// single-code distance trees), <0 for an over-subscribed set.
int construct(Huffman& h, const uint16_t* lengths, int n) {
  std::memset(h.count, 0, sizeof h.count);
  for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
  if (h.count[0] == n) return 0;
  int left = 1;
  for (int len = 1; len < 16; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return left;
  }
  uint16_t offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
  for (int i = 0; i < n; ++i)
    if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
  return left;
}

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

InflateResult codes(State& s, const Huffman& lencode, const Huffman& distcode) {
  for (;;) {
    int symbol = decode(s, lencode);
    if (s.truncated) return InflateResult::Truncated;
    if (symbol < 0) return InflateResult::BadData;
    if (symbol < 256) {
      if (s.out_pos >= s.out_capacity) return InflateResult::OutputFull;
      s.out[s.out_pos++] = static_cast<uint8_t>(symbol);
    } else if (symbol == 256) {
      return InflateResult::Ok;
    } else {
      symbol -= 257;
      if (symbol >= 29) return InflateResult::BadData;
      const size_t len = kLenBase[symbol] + bits(s, kLenExtra[symbol]);
      const int dsym = decode(s, distcode);
      if (s.truncated) return InflateResult::Truncated;
      if (dsym < 0 || dsym >= 30) return InflateResult::BadData;
      const size_t dist = kDistBase[dsym] + bits(s, kDistExtra[dsym]);
      if (s.truncated) return InflateResult::Truncated;
      if (dist > s.out_pos) return InflateResult::BadData;
      // A match that does not fit still fills the buffer (callers that want
      // only the beginning of a stream use what was produced).
      const size_t room = s.out_capacity - s.out_pos, n = len < room ? len : room;
      for (size_t i = 0; i < n; ++i, ++s.out_pos) s.out[s.out_pos] = s.out[s.out_pos - dist];
      if (n < len) return InflateResult::OutputFull;
    }
  }
}

InflateResult stored(State& s) {
  s.bit_buf = 0;
  s.bit_count = 0;
  if (s.in_size - s.in_pos < 4) return InflateResult::Truncated;
  const unsigned len = s.in[s.in_pos] | s.in[s.in_pos + 1] << 8;
  const unsigned nlen = s.in[s.in_pos + 2] | s.in[s.in_pos + 3] << 8;
  s.in_pos += 4;
  if (len != (~nlen & 0xFFFF)) return InflateResult::BadData;
  if (s.in_size - s.in_pos < len) return InflateResult::Truncated;
  if (s.out_capacity - s.out_pos < len) {
    const size_t room = s.out_capacity - s.out_pos;
    std::memcpy(s.out + s.out_pos, s.in + s.in_pos, room);
    s.out_pos += room;
    return InflateResult::OutputFull;
  }
  std::memcpy(s.out + s.out_pos, s.in + s.in_pos, len);
  s.in_pos += len;
  s.out_pos += len;
  return InflateResult::Ok;
}

InflateResult fixed(State& s) {
  static Huffman lencode, distcode;
  static const bool built = [] {
    uint16_t lengths[288];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    construct(lencode, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    construct(distcode, lengths, 30);
    return true;
  }();
  (void)built;
  return codes(s, lencode, distcode);
}

InflateResult dynamic(State& s) {
  static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  const int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
  if (s.truncated) return InflateResult::Truncated;
  if (nlen > 286 || ndist > 30) return InflateResult::BadData;
  uint16_t lengths[320] = {};
  for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<uint16_t>(bits(s, 3));
  if (s.truncated) return InflateResult::Truncated;
  Huffman lencode, distcode;
  if (construct(lencode, lengths, 19) != 0) return InflateResult::BadData;
  int index = 0;
  while (index < nlen + ndist) {
    int symbol = decode(s, lencode);
    if (s.truncated) return InflateResult::Truncated;
    if (symbol < 0) return InflateResult::BadData;
    if (symbol < 16) {
      lengths[index++] = static_cast<uint16_t>(symbol);
    } else {
      uint16_t len = 0;
      if (symbol == 16) {
        if (index == 0) return InflateResult::BadData;
        len = lengths[index - 1];
        symbol = 3 + bits(s, 2);
      } else if (symbol == 17) {
        symbol = 3 + bits(s, 3);
      } else {
        symbol = 11 + bits(s, 7);
      }
      if (s.truncated) return InflateResult::Truncated;
      if (index + symbol > nlen + ndist) return InflateResult::BadData;
      while (symbol--) lengths[index++] = len;
    }
  }
  if (lengths[256] == 0) return InflateResult::BadData;
  const int err = construct(lencode, lengths, nlen);
  if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return InflateResult::BadData;
  const int derr = construct(distcode, lengths + nlen, ndist);
  if (derr < 0 || (derr > 0 && ndist - distcode.count[0] != 1)) return InflateResult::BadData;
  return codes(s, lencode, distcode);
}

}  // namespace

InflateResult inflate_raw(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t& out_size) {
  State s{in, in_size};
  s.out = out;
  s.out_capacity = out_capacity;
  InflateResult r = InflateResult::Ok;
  int last;
  do {
    last = bits(s, 1);
    const int type = bits(s, 2);
    if (s.truncated) {
      r = InflateResult::Truncated;
      break;
    }
    r = type == 0 ? stored(s) : type == 1 ? fixed(s) : type == 2 ? dynamic(s) : InflateResult::BadData;
  } while (r == InflateResult::Ok && !last);
  out_size = s.out_pos;
  return r;
}

InflateResult inflate_auto(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_capacity, size_t& out_size) {
  // zlib header: CM = 8 (deflate), CINFO <= 7, no preset dictionary, check bits.
  if (in_size >= 2 && (in[0] & 0x0F) == 8 && (in[0] >> 4) <= 7 && !(in[1] & 0x20) && ((in[0] << 8) | in[1]) % 31 == 0)
    return inflate_raw(in + 2, in_size - 2, out, out_capacity, out_size);
  return inflate_raw(in, in_size, out, out_capacity, out_size);
}

const char* inflate_result_name(InflateResult r) {
  switch (r) {
    case InflateResult::Ok: return "ok";
    case InflateResult::OutputFull: return "output buffer too small";
    case InflateResult::BadData: return "invalid DEFLATE data";
    case InflateResult::Truncated: return "truncated input";
  }
  return "?";
}

}  // namespace xbem
