#include "gpu/root_signature.h"

#include <array>
#include <cstring>
#include <format>

namespace xbem::gpu {
namespace {

constexpr uint32_t kMaxParams = 64;     // D3D12 limit on root signature size is 64 DWORDs
constexpr uint32_t kMaxRanges = 4096;
constexpr uint32_t kMaxSamplers = 2048;

uint32_t fourcc(const char* s) {
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
         uint32_t(uint8_t(s[3])) << 24;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
  const size_t at = out.size();
  out.resize(at + 4);
  std::memcpy(out.data() + at, &v, 4);
}
void set32(std::vector<uint8_t>& out, size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); }

bool get32(const uint8_t* data, size_t size, size_t at, uint32_t& v) {
  if (at > size || size - at < 4) return false;
  std::memcpy(&v, data + at, 4);
  return true;
}

}  // namespace

std::string RootSignature::summary() const {
  std::string s = std::format("v1.{} flags {:#x}, {} params:", version - 1, flags, params.size());
  for (const Param& p : params) {
    switch (p.type) {
      case ParamType::DescriptorTable: {
        s += std::format(" table[{}", p.ranges.size());
        for (const Range& r : p.ranges) s += std::format(" {}{}x{}", "TUBS"[r.type & 3], r.base_register, r.count);
        s += "]";
        break;
      }
      case ParamType::Constants32: s += std::format(" const(b{} x{})", p.shader_register, p.num_constants); break;
      case ParamType::Cbv: s += std::format(" cbv(b{})", p.shader_register); break;
      case ParamType::Srv: s += std::format(" srv(t{})", p.shader_register); break;
      case ParamType::Uav: s += std::format(" uav(u{})", p.shader_register); break;
    }
  }
  if (!static_samplers.empty()) s += std::format(", {} static samplers", static_samplers.size());
  return s;
}

bool read_root_signature_desc(const MemoryReader& read, uint64_t address, uint32_t version_override,
                              RootSignature& out, std::string& error) {
  out = RootSignature{};
  uint64_t desc = address;
  uint32_t version = version_override;
  if (!version) {
    if (!read(address, &version, 4)) return error = "unreadable versioned description", false;
    desc = address + 8;  // the union follows the (padded) version field
  }
  if (version < 1 || version > 3) return error = std::format("unknown root signature version {}", version), false;
  out.version = version;

  // D3D12_ROOT_SIGNATURE_DESC{,1,2}: NumParameters @0, pParameters @8,
  // NumStaticSamplers @16, pStaticSamplers @24, Flags @32.
  uint8_t d[40];
  if (!read(desc, d, sizeof d)) return error = "unreadable description", false;
  uint32_t num_params, num_samplers;
  uint64_t params, samplers;
  std::memcpy(&num_params, d + 0, 4);
  std::memcpy(&params, d + 8, 8);
  std::memcpy(&num_samplers, d + 16, 4);
  std::memcpy(&samplers, d + 24, 8);
  std::memcpy(&out.flags, d + 32, 4);
  if (num_params > kMaxParams || num_samplers > kMaxSamplers)
    return error = std::format("implausible counts ({} parameters, {} samplers)", num_params, num_samplers), false;

  const size_t range_size = version == 1 ? 20 : 24;
  for (uint32_t i = 0; i < num_params; ++i) {
    uint8_t p[32];  // D3D12_ROOT_PARAMETER{,1}: type @0, union @8, visibility @24
    if (!read(params + i * 32ull, p, sizeof p)) return error = std::format("unreadable parameter {}", i), false;
    RootSignature::Param param;
    uint32_t type;
    std::memcpy(&type, p, 4);
    if (type > 4) return error = std::format("parameter {} has unknown type {}", i, type), false;
    param.type = static_cast<RootSignature::ParamType>(type);
    std::memcpy(&param.visibility, p + 24, 4);
    if (param.type == RootSignature::ParamType::DescriptorTable) {
      uint32_t num_ranges;
      uint64_t ranges;
      std::memcpy(&num_ranges, p + 8, 4);
      std::memcpy(&ranges, p + 16, 8);
      if (num_ranges > kMaxRanges) return error = std::format("parameter {}: {} ranges", i, num_ranges), false;
      for (uint32_t r = 0; r < num_ranges; ++r) {
        uint32_t w[6] = {};
        if (!read(ranges + r * range_size, w, range_size))
          return error = std::format("parameter {}: unreadable range {}", i, r), false;
        RootSignature::Range range{w[0], w[1], w[2], w[3], 0, 0};
        if (version == 1) {
          range.offset = w[4];
        } else {
          range.flags = w[4];
          range.offset = w[5];
        }
        param.ranges.push_back(range);
      }
    } else if (param.type == RootSignature::ParamType::Constants32) {
      std::memcpy(&param.shader_register, p + 8, 4);
      std::memcpy(&param.space, p + 12, 4);
      std::memcpy(&param.num_constants, p + 16, 4);
    } else {
      std::memcpy(&param.shader_register, p + 8, 4);
      std::memcpy(&param.space, p + 12, 4);
      if (version >= 2) std::memcpy(&param.flags, p + 16, 4);
    }
    out.params.push_back(std::move(param));
  }

  const size_t sampler_size = version == 3 ? 56 : 52;
  for (uint32_t i = 0; i < num_samplers; ++i) {
    std::array<uint32_t, 14> s{};
    if (!read(samplers + i * sampler_size, s.data(), sampler_size))
      return error = std::format("unreadable static sampler {}", i), false;
    out.static_samplers.push_back(s);
  }
  return true;
}

std::vector<uint8_t> serialize_root_signature(const RootSignature& rs) {
  // RTS0 part: header, parameter headers, payloads, static samplers. Offsets
  // are relative to the start of the part.
  std::vector<uint8_t> part;
  put32(part, rs.version);
  put32(part, static_cast<uint32_t>(rs.params.size()));
  put32(part, 24);
  put32(part, static_cast<uint32_t>(rs.static_samplers.size()));
  put32(part, 0);  // static sampler offset, patched below
  put32(part, rs.flags);
  const size_t headers = part.size();
  for (const auto& p : rs.params) {
    put32(part, static_cast<uint32_t>(p.type));
    put32(part, p.visibility);
    put32(part, 0);  // payload offset, patched below
  }
  for (size_t i = 0; i < rs.params.size(); ++i) {
    const auto& p = rs.params[i];
    set32(part, headers + i * 12 + 8, static_cast<uint32_t>(part.size()));
    switch (p.type) {
      case RootSignature::ParamType::DescriptorTable: {
        put32(part, static_cast<uint32_t>(p.ranges.size()));
        put32(part, static_cast<uint32_t>(part.size() + 4));
        for (const auto& r : p.ranges) {
          put32(part, r.type);
          put32(part, r.count);
          put32(part, r.base_register);
          put32(part, r.space);
          if (rs.version >= 2) put32(part, r.flags);
          put32(part, r.offset);
        }
        break;
      }
      case RootSignature::ParamType::Constants32:
        put32(part, p.shader_register);
        put32(part, p.space);
        put32(part, p.num_constants);
        break;
      default:
        put32(part, p.shader_register);
        put32(part, p.space);
        if (rs.version >= 2) put32(part, p.flags);
        break;
    }
  }
  set32(part, 16, static_cast<uint32_t>(part.size()));
  const size_t sampler_words = rs.version == 3 ? 14 : 13;
  for (const auto& s : rs.static_samplers)
    for (size_t w = 0; w < sampler_words; ++w) put32(part, s[w]);

  // DXBC container: magic, 16-byte checksum (left zero: only this emulator
  // reads the blob back), version 1, total size, part count, part offsets,
  // then each part as fourcc, size, data.
  std::vector<uint8_t> out;
  put32(out, fourcc("DXBC"));
  for (int i = 0; i < 4; ++i) put32(out, 0);
  put32(out, 1);
  put32(out, 0);  // total size, patched below
  put32(out, 1);
  put32(out, static_cast<uint32_t>(out.size() + 4));
  put32(out, fourcc("RTS0"));
  put32(out, static_cast<uint32_t>(part.size()));
  out.insert(out.end(), part.begin(), part.end());
  set32(out, 24, static_cast<uint32_t>(out.size()));
  return out;
}

bool deserialize_root_signature(const uint8_t* data, size_t size, RootSignature& out, std::string& error) {
  out = RootSignature{};
  uint32_t magic;
  if (!data || !get32(data, size, 0, magic)) return error = "empty blob", false;
  const uint8_t* part = data;
  size_t part_size = size;
  if (magic == fourcc("DXBC")) {
    uint32_t count;
    if (!get32(data, size, 28, count) || count > 64) return error = "bad DXBC header", false;
    part = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t offset, cc, len;
      if (!get32(data, size, 32 + i * 4, offset) || !get32(data, size, offset, cc) ||
          !get32(data, size, offset + 4, len) || offset + 8ull + len > size)
        return error = "bad DXBC part table", false;
      if (cc == fourcc("RTS0")) {
        part = data + offset + 8;
        part_size = len;
        break;
      }
    }
    if (!part) return error = "no RTS0 part", false;
  }
  uint32_t h[6];
  for (int i = 0; i < 6; ++i)
    if (!get32(part, part_size, i * 4, h[i])) return error = "short RTS0 header", false;
  out.version = h[0];
  out.flags = h[5];
  if (out.version < 1 || out.version > 3) return error = std::format("unknown RTS0 version {}", out.version), false;
  if (h[1] > kMaxParams || h[3] > kMaxSamplers) return error = "implausible RTS0 counts", false;
  for (uint32_t i = 0; i < h[1]; ++i) {
    uint32_t type, vis, payload;
    const size_t at = h[2] + i * 12;
    if (!get32(part, part_size, at, type) || !get32(part, part_size, at + 4, vis) ||
        !get32(part, part_size, at + 8, payload) || type > 4)
      return error = std::format("bad parameter {}", i), false;
    RootSignature::Param p;
    p.type = static_cast<RootSignature::ParamType>(type);
    p.visibility = vis;
    bool ok = true;
    if (p.type == RootSignature::ParamType::DescriptorTable) {
      uint32_t n, ranges;
      ok = get32(part, part_size, payload, n) && get32(part, part_size, payload + 4, ranges) && n <= kMaxRanges;
      const size_t stride = out.version >= 2 ? 24 : 20;
      for (uint32_t r = 0; ok && r < n; ++r) {
        uint32_t w[6] = {};
        for (size_t k = 0; ok && k < stride / 4; ++k) ok = get32(part, part_size, ranges + r * stride + k * 4, w[k]);
        RootSignature::Range range{w[0], w[1], w[2], w[3], 0, 0};
        if (out.version >= 2) {
          range.flags = w[4];
          range.offset = w[5];
        } else {
          range.offset = w[4];
        }
        p.ranges.push_back(range);
      }
    } else if (p.type == RootSignature::ParamType::Constants32) {
      ok = get32(part, part_size, payload, p.shader_register) && get32(part, part_size, payload + 4, p.space) &&
           get32(part, part_size, payload + 8, p.num_constants);
    } else {
      ok = get32(part, part_size, payload, p.shader_register) && get32(part, part_size, payload + 4, p.space) &&
           (out.version < 2 || get32(part, part_size, payload + 8, p.flags));
    }
    if (!ok) return error = std::format("bad payload of parameter {}", i), false;
    out.params.push_back(std::move(p));
  }
  const size_t words = out.version == 3 ? 14 : 13;
  for (uint32_t i = 0; i < h[3]; ++i) {
    std::array<uint32_t, 14> s{};
    for (size_t w = 0; w < words; ++w)
      if (!get32(part, part_size, h[4] + (i * words + w) * 4, s[w])) return error = "bad static sampler", false;
    out.static_samplers.push_back(s);
  }
  return true;
}

void build_root_signature_desc_1_0(const RootSignature& rs, std::vector<uint8_t>& buffer) {
  size_t total_ranges = 0;
  for (const auto& p : rs.params) total_ranges += p.ranges.size();
  const size_t params_at = 40;
  const size_t ranges_at = params_at + rs.params.size() * 32;
  const size_t samplers_at = ranges_at + total_ranges * 20;
  buffer.assign(samplers_at + rs.static_samplers.size() * 52, 0);
  uint8_t* base = buffer.data();
  auto put = [&](size_t at, const void* v, size_t n) { std::memcpy(base + at, v, n); };
  const auto addr = [&](size_t at) { return reinterpret_cast<uint64_t>(base + at); };

  const uint32_t num_params = static_cast<uint32_t>(rs.params.size());
  const uint32_t num_samplers = static_cast<uint32_t>(rs.static_samplers.size());
  const uint64_t p_params = num_params ? addr(params_at) : 0, p_samplers = num_samplers ? addr(samplers_at) : 0;
  put(0, &num_params, 4);
  put(8, &p_params, 8);
  put(16, &num_samplers, 4);
  put(24, &p_samplers, 8);
  put(32, &rs.flags, 4);
  size_t next_range = ranges_at;
  for (size_t i = 0; i < rs.params.size(); ++i) {
    const auto& p = rs.params[i];
    const size_t at = params_at + i * 32;
    const uint32_t type = static_cast<uint32_t>(p.type);
    put(at, &type, 4);
    put(at + 24, &p.visibility, 4);
    if (p.type == RootSignature::ParamType::DescriptorTable) {
      const uint32_t n = static_cast<uint32_t>(p.ranges.size());
      const uint64_t ranges = n ? addr(next_range) : 0;
      put(at + 8, &n, 4);
      put(at + 16, &ranges, 8);
      for (const auto& r : p.ranges) {
        const uint32_t w[5] = {r.type, r.count, r.base_register, r.space, r.offset};
        put(next_range, w, 20);
        next_range += 20;
      }
    } else {
      put(at + 8, &p.shader_register, 4);
      put(at + 12, &p.space, 4);
      if (p.type == RootSignature::ParamType::Constants32) put(at + 16, &p.num_constants, 4);
    }
  }
  for (size_t i = 0; i < rs.static_samplers.size(); ++i) put(samplers_at + i * 52, rs.static_samplers[i].data(), 52);
}

}  // namespace xbem::gpu
