#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace spio::platform
{

struct MtlsIdentity
{
  std::string role;
  std::string tenant_id;
  std::string node_id;
};

std::optional<MtlsIdentity> ParseMtlsUriSan(std::string_view value);
bool IsPlatformServiceRole(std::string_view role);
nlohmann::json SerializeMtlsIdentity(const MtlsIdentity &identity);

}  // namespace spio::platform
