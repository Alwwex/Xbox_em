#include "gpu/backend.h"

namespace xbem::gpu {

namespace {

class NullBackend final : public Backend {
 public:
  BackendInfo info() const override {
    return {"null", false,
            "No rendering. D3D12 device creation reports DXGI_ERROR_UNSUPPORTED so titles fail "
            "gracefully at graphics init. A Vulkan backend is the next milestone."};
  }
  bool initialize(std::string&) override { return true; }
};

}  // namespace

std::unique_ptr<Backend> create_backend(std::string_view name) {
  if (name == "null") return std::make_unique<NullBackend>();
  return nullptr;
}

Backend& active_backend() {
  static std::unique_ptr<Backend> backend = [] {
    auto b = create_backend("null");
    std::string error;
    b->initialize(error);
    return b;
  }();
  return *backend;
}

}  // namespace xbem::gpu
