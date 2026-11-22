#pragma once

#include <string>
#include <vector>

namespace xbem::platform {

// Xbox Series X|S use a custom AMD Zen 2 CPU. Because xbem executes guest code
// natively, every instruction-set extension the game was compiled for must also
// exist on the host CPU.
struct CpuFeature {
  const char* name;
  bool present;
  bool important;  // likely to be used by code compiled for Zen 2
};

std::string host_cpu_brand();
std::vector<CpuFeature> host_cpu_features();
bool host_has_feature(const char* name);

}  // namespace xbem::platform
