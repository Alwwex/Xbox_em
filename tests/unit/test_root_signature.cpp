// Root signature descriptions -> DXBC/RTS0 -> parsed form and back.

#include <cstring>
#include <string>
#include <vector>

#include "gpu/root_signature.h"
#include "test.h"

using namespace xbem::gpu;

namespace {

// Host memory stands in for guest memory: the structures below are laid out
// exactly like the x64 D3D12 ones.
const MemoryReader kRead = [](uint64_t a, void* out, size_t n) {
  if (!a) return false;
  std::memcpy(out, reinterpret_cast<const void*>(a), n);
  return true;
};

struct Range1 {
  uint32_t type, count, base, space, flags, offset;
};
struct Param {
  uint32_t type, pad;
  uint64_t a, b;  // union: table {count, ranges} / constants {reg, space, num} / descriptor {reg, space, flags}
  uint32_t visibility, pad2;
};
struct Desc {
  uint32_t num_params, pad0;
  uint64_t params;
  uint32_t num_samplers, pad1;
  uint64_t samplers;
  uint32_t flags, pad2;
};
struct Versioned {
  uint32_t version, pad;
  Desc desc;
};
static_assert(sizeof(Param) == 32 && sizeof(Desc) == 40 && sizeof(Range1) == 24);

}  // namespace

TEST(root_signature_v11_roundtrip) {
  Range1 ranges[2] = {{0 /*SRV*/, 8, 0, 0, 0x4, 0}, {1 /*UAV*/, 2, 1, 0, 0, 0xFFFFFFFF}};
  Param params[3] = {};
  params[0].type = 0;  // table
  params[0].a = 2;
  params[0].b = reinterpret_cast<uint64_t>(ranges);
  params[1].type = 1;  // 32-bit constants b3, space 1, 4 values
  params[1].a = 3ull | 1ull << 32;
  params[1].b = 4;
  params[1].visibility = 5;  // pixel
  params[2].type = 2;        // CBV b0, flags 0x8
  params[2].a = 0;
  params[2].b = 8;
  uint32_t sampler[13] = {0x15, 1, 1, 1, 0, 16, 0, 0, 0, 0x7f7fffff, 2, 0, 0};
  Versioned v{2, 0, {3, 0, reinterpret_cast<uint64_t>(params), 1, 0, reinterpret_cast<uint64_t>(sampler), 0x03000001, 0}};

  RootSignature rs;
  std::string error;
  CHECK(read_root_signature_desc(kRead, reinterpret_cast<uint64_t>(&v), 0, rs, error));
  CHECK(rs.version == 2 && rs.flags == 0x03000001 && rs.params.size() == 3);
  CHECK(rs.params[0].ranges.size() == 2 && rs.params[0].ranges[0].count == 8 && rs.params[0].ranges[0].flags == 4);
  CHECK(rs.params[0].ranges[1].offset == 0xFFFFFFFF);
  CHECK(rs.params[1].shader_register == 3 && rs.params[1].space == 1 && rs.params[1].num_constants == 4);
  CHECK(rs.params[1].visibility == 5 && rs.params[2].flags == 8);
  CHECK(rs.static_samplers.size() == 1 && rs.static_samplers[0][10] == 2);

  const std::vector<uint8_t> blob = serialize_root_signature(rs);
  CHECK(blob.size() > 40 && std::memcmp(blob.data(), "DXBC", 4) == 0);
  uint32_t total;
  std::memcpy(&total, blob.data() + 24, 4);
  CHECK(total == blob.size());

  RootSignature back;
  CHECK(deserialize_root_signature(blob.data(), blob.size(), back, error));
  CHECK(back.version == 2 && back.flags == rs.flags && back.params.size() == 3);
  CHECK(back.params[0].ranges.size() == 2 && back.params[0].ranges[1].type == 1 &&
        back.params[0].ranges[1].offset == 0xFFFFFFFF && back.params[0].ranges[0].flags == 4);
  CHECK(back.params[1].num_constants == 4 && back.params[2].flags == 8);
  CHECK(back.static_samplers.size() == 1 && back.static_samplers[0][9] == 0x7f7fffff);
  CHECK(back.summary() == rs.summary());

  // The 1.0 description the deserializer hands out.
  std::vector<uint8_t> desc;
  build_root_signature_desc_1_0(back, desc);
  const auto* d = reinterpret_cast<const Desc*>(desc.data());
  CHECK(d->num_params == 3 && d->num_samplers == 1 && d->flags == 0x03000001);
  const auto* p = reinterpret_cast<const Param*>(d->params);
  CHECK(p[0].type == 0 && static_cast<uint32_t>(p[0].a) == 2);
  const auto* r = reinterpret_cast<const uint32_t*>(p[0].b);  // 1.0 ranges: 5 words
  CHECK(r[0] == 0 && r[1] == 8 && r[4] == 0 && r[5] == 1 && r[9] == 0xFFFFFFFF);
  CHECK(p[1].visibility == 5 && static_cast<uint32_t>(p[1].b) == 4);
}

TEST(root_signature_v10_and_errors) {
  uint32_t ranges[5] = {3 /*SAMPLER*/, 4, 0, 0, 0};
  Param param{};
  param.type = 0;
  param.a = 1;
  param.b = reinterpret_cast<uint64_t>(ranges);
  Desc desc{1, 0, reinterpret_cast<uint64_t>(&param), 0, 0, 0, 0, 0};
  RootSignature rs;
  std::string error;
  // D3D12SerializeRootSignature: a bare 1.0 description.
  CHECK(read_root_signature_desc(kRead, reinterpret_cast<uint64_t>(&desc), 1, rs, error));
  CHECK(rs.version == 1 && rs.params.size() == 1 && rs.params[0].ranges[0].type == 3);
  const std::vector<uint8_t> blob = serialize_root_signature(rs);
  RootSignature back;
  CHECK(deserialize_root_signature(blob.data(), blob.size(), back, error) && back.params[0].ranges[0].count == 4);

  Versioned bad{7, 0, desc};
  CHECK(!read_root_signature_desc(kRead, reinterpret_cast<uint64_t>(&bad), 0, rs, error));
  CHECK(!error.empty());
  const uint8_t junk[8] = {'D', 'X', 'B', 'C', 0, 0, 0, 0};
  CHECK(!deserialize_root_signature(junk, sizeof junk, back, error));
}
