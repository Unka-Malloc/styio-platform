#include "PlatformService/Identity.hpp"

#include <array>
#include <sstream>
#include <vector>

namespace spio::platform
{

namespace
{

std::vector<std::string> SplitPath(std::string_view input)
{
  std::vector<std::string> parts;
  std::stringstream stream{std::string(input)};
  std::string item;
  while (std::getline(stream, item, '/'))
  {
    if (!item.empty())
    {
      parts.push_back(item);
    }
  }
  return parts;
}

std::string ValueAfter(const std::vector<std::string> &parts, std::string_view key)
{
  for (size_t index = 0; index + 1 < parts.size(); ++index)
  {
    if (parts[index] == key)
    {
      return parts[index + 1];
    }
  }
  return {};
}

}  // namespace

bool IsPlatformServiceRole(std::string_view role)
{
  static constexpr std::array<std::string_view, 6> kRoles = {
      "control-plane",
      "worker",
      "registry-writer",
      "mirror",
      "operator",
      "cluster-registrar",
  };
  for (const std::string_view candidate : kRoles)
  {
    if (role == candidate)
    {
      return true;
    }
  }
  return false;
}

std::optional<MtlsIdentity> ParseMtlsUriSan(std::string_view value)
{
  constexpr std::string_view prefix = "spiffe://styio-platform/";
  if (!value.starts_with(prefix))
  {
    return std::nullopt;
  }
  const std::vector<std::string> parts = SplitPath(value.substr(prefix.size()));
  MtlsIdentity identity{
      .role = ValueAfter(parts, "role"),
      .tenant_id = ValueAfter(parts, "tenant"),
      .node_id = ValueAfter(parts, "node"),
  };
  if (!IsPlatformServiceRole(identity.role) || identity.node_id.empty())
  {
    return std::nullopt;
  }
  return identity;
}

nlohmann::json SerializeMtlsIdentity(const MtlsIdentity &identity)
{
  return {
      {"role", identity.role},
      {"tenant_id", identity.tenant_id},
      {"node_id", identity.node_id},
  };
}

}  // namespace spio::platform
