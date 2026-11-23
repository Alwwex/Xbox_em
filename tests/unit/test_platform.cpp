#include <cstdint>
#include <cstring>

#include "platform/memory.h"
#include "test.h"

using namespace xbem;

TEST(safe_read_valid_and_invalid) {
  const char text[] = "guid-or-string";
  char out[sizeof text] = {};
  CHECK(platform::safe_read(reinterpret_cast<uint64_t>(text), out, sizeof text));
  CHECK(std::memcmp(out, text, sizeof text) == 0);
  CHECK(!platform::safe_read(0x10, out, 8));                     // unmapped low page
  void* p = platform::map_anywhere(platform::page_size(), platform::page_size(), platform::Prot::None);
  CHECK(p != nullptr);
  CHECK(!platform::safe_read(reinterpret_cast<uint64_t>(p), out, 8));  // mapped but inaccessible
  platform::unmap(p, platform::page_size());
}
