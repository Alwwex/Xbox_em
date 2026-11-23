#include "hle/registry.h"

#include "common/strings.h"

namespace xbem::hle {

std::string Registry::normalize_dll_name(std::string_view dll_name) {
  // Strip any directory part.
  const size_t slash = dll_name.find_last_of("\\/");
  if (slash != std::string_view::npos) dll_name = dll_name.substr(slash + 1);
  std::string n = to_lower(dll_name);
  if (n.find('.') == std::string::npos) n += ".dll";
  return n;
}

bool Registry::is_api_set(std::string_view dll_name) {
  return istarts_with(dll_name, "api-ms-win-") || istarts_with(dll_name, "ext-ms-win-");
}

const Registry& Registry::instance() {
  static const Registry registry;
  return registry;
}

Registry::Registry() {
  modules_.push_back(make_kernelx_module());
  modules_.push_back(make_xgameruntime_module());
  modules_.push_back(make_d3d12_module());
  modules_.push_back(make_ucrt_module());
  modules_.push_back(make_oleaut32_module());
  modules_.push_back(make_bcrypt_module());

  export_index_.resize(modules_.size());
  for (size_t i = 0; i < modules_.size(); ++i) {
    const ModuleDef& m = modules_[i];
    by_name_[normalize_dll_name(m.name)] = i;
    for (const std::string& alias : m.aliases) by_name_[normalize_dll_name(alias)] = i;
    for (size_t e = 0; e < m.exports.size(); ++e) export_index_[i].emplace(std::string(m.exports[e].name), e);
    if (m.name == "kernelx.dll") api_set_target_ = &modules_[i];
    if (m.name == "ucrtbase.dll") crt_target_ = &modules_[i];
  }
}

const ModuleDef* Registry::find_module(std::string_view dll_name) const {
  const std::string n = normalize_dll_name(dll_name);
  if (auto it = by_name_.find(n); it != by_name_.end()) return &modules_[it->second];
  // API sets (api-ms-win-core-synch-l1-2-0.dll, ...) are virtual DLL names that
  // the Windows loader redirects to a host DLL. We serve them all from kernelx.
  // The C runtime API sets (api-ms-win-crt-*) are served by ucrtbase.dll.
  if (istarts_with(n, "api-ms-win-crt-")) return crt_target_;
  if (is_api_set(n)) return api_set_target_;
  return nullptr;
}

Registry::Lookup Registry::find(std::string_view dll_name, std::string_view function) const {
  Lookup l;
  l.module = find_module(dll_name);
  if (!l.module) return l;
  const size_t mi = static_cast<size_t>(l.module - modules_.data());
  const auto& idx = export_index_[mi];
  if (auto it = idx.find(std::string(function)); it != idx.end()) l.exp = &l.module->exports[it->second];
  return l;
}

}  // namespace xbem::hle
