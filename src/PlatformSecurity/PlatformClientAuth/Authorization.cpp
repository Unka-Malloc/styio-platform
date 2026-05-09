#include "PlatformSecurity/PlatformClientAuth/Authorization.hpp"

#include <initializer_list>

namespace spio::platform
{

namespace
{

bool RoleIn(const MtlsIdentity &identity, std::initializer_list<std::string_view> allowed)
{
  for (const std::string_view role : allowed)
  {
    if (identity.role == role)
    {
      return true;
    }
  }
  return false;
}

}  // namespace

bool IsInternalRole(const MtlsIdentity &identity)
{
  return identity.role == "control-plane" || identity.role == "worker" || identity.role == "mirror" ||
         identity.role == "registry-writer" || identity.role == "operator" || identity.role == "cluster-registrar" ||
         identity.role == "package-owner";
}

bool IsAuthorizedForOperation(std::string_view operation_id, const MtlsIdentity &identity)
{
  if (operation_id == "registryStatus")
  {
    return RoleIn(identity, {"control-plane", "registry-writer", "mirror", "operator"});
  }
  if (operation_id == "registryDescriptor")
  {
    return RoleIn(identity, {"control-plane", "registry-writer", "mirror", "operator"});
  }
  if (operation_id == "publishRelease")
  {
    return RoleIn(identity, {"registry-writer", "operator", "package-owner"});
  }
  if (operation_id == "verifyRegistry")
  {
    return RoleIn(identity, {"registry-writer", "mirror", "operator"});
  }
  if (operation_id == "getPackage" || operation_id == "listPackageReleases" || operation_id == "getPackageRelease" ||
      operation_id == "listPackageOwners" || operation_id == "listRepositories" ||
      operation_id == "listRepositoryVersions" || operation_id == "getPublication" ||
      operation_id == "listDistributions")
  {
    return RoleIn(identity, {"control-plane", "registry-writer", "mirror", "operator", "package-owner"});
  }
  if (operation_id == "yankPackageRelease" || operation_id == "unyankPackageRelease" ||
      operation_id == "addPackageOwner" || operation_id == "removePackageOwner")
  {
    return RoleIn(identity, {"operator", "package-owner", "registry-writer"});
  }
  if (operation_id == "createPublishToken" || operation_id == "listPublishTokens" ||
      operation_id == "revokePublishToken")
  {
    return RoleIn(identity, {"operator", "package-owner", "registry-writer"});
  }
  if (operation_id == "promoteDistribution" || operation_id == "rollbackDistribution")
  {
    return RoleIn(identity, {"operator"});
  }
  if (operation_id == "verifyPublication")
  {
    return RoleIn(identity, {"operator", "mirror", "registry-writer"});
  }
  if (operation_id == "mirrorStatus")
  {
    return RoleIn(identity, {"control-plane", "registry-writer", "mirror", "operator"});
  }
  if (operation_id == "registerWorkgroupCluster")
  {
    return RoleIn(identity, {"control-plane", "operator", "cluster-registrar"});
  }
  if (operation_id == "listWorkgroupClusters")
  {
    return IsInternalRole(identity);
  }
  return true;
}

}  // namespace spio::platform
