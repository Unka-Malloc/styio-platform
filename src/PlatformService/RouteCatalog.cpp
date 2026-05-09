#include "PlatformService/RouteCatalog.hpp"

#include <algorithm>
#include <array>

namespace spio::platform
{

bool IsRegistryOperation(std::string_view operation_id)
{
  static constexpr std::array<std::string_view, 25> kRegistryOperations = {
      "registryStatus",
      "registryDescriptor",
      "publishRelease",
      "verifyRegistry",
      "getPackage",
      "listPackageReleases",
      "getPackageRelease",
      "yankPackageRelease",
      "unyankPackageRelease",
      "listPackageOwners",
      "addPackageOwner",
      "removePackageOwner",
      "createPublishToken",
      "listPublishTokens",
      "revokePublishToken",
      "listRepositories",
      "listRepositoryVersions",
      "getPublication",
      "verifyPublication",
      "listDistributions",
      "promoteDistribution",
      "rollbackDistribution",
      "listReleaseChannels",
      "rolloutReleaseChannel",
      "mirrorStatus",
  };
  return std::find(kRegistryOperations.begin(), kRegistryOperations.end(), operation_id) != kRegistryOperations.end();
}

bool IsRegistryTokenCapableOperation(std::string_view operation_id)
{
  return operation_id == "publishRelease" || operation_id == "yankPackageRelease" ||
         operation_id == "unyankPackageRelease" || operation_id == "addPackageOwner" ||
         operation_id == "removePackageOwner" || operation_id == "promoteDistribution" ||
         operation_id == "rollbackDistribution";
}

std::vector<RouteSpec> BuildPlatformControlPlaneRoutes()
{
  return {
      {.operation_id = "health", .method = HttpMethod::Get, .path = "/health"},
      {.operation_id = "nodeSelf", .method = HttpMethod::Get, .path = "/nodes/self"},
      {.operation_id = "submitJob", .method = HttpMethod::Post, .path = "/jobs"},
      {.operation_id = "getJob", .method = HttpMethod::Get, .path = "/jobs/{job_id}"},
      {.operation_id = "getJobEvents", .method = HttpMethod::Get, .path = "/jobs/{job_id}/events"},
      {.operation_id = "cancelJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/cancel"},
      {.operation_id = "registerWorker", .method = HttpMethod::Post, .path = "/workers/register", .internal = true},
      {.operation_id = "registerCompileContainer", .method = HttpMethod::Post, .path = "/compile-containers/register", .internal = true},
      {.operation_id = "getCompileContainer", .method = HttpMethod::Get, .path = "/compile-containers/{container_id}", .internal = true},
      {.operation_id = "switchCompileContainerWorkspace", .method = HttpMethod::Post, .path = "/compile-containers/{container_id}/switch-workspace", .internal = true},
      {.operation_id = "claimJob", .method = HttpMethod::Post, .path = "/jobs/claim", .internal = true},
      {.operation_id = "heartbeatJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/heartbeat", .internal = true},
      {.operation_id = "completeJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/complete", .internal = true},
      {.operation_id = "registerWorkgroupCluster", .method = HttpMethod::Post, .path = "/workgroups/{workgroup_id}/clusters/register", .internal = true},
      {.operation_id = "listWorkgroupClusters", .method = HttpMethod::Get, .path = "/workgroups/{workgroup_id}/clusters", .internal = true},
      {.operation_id = "mirrorStatus", .method = HttpMethod::Get, .path = "/mirrors/{mirror_id}/status"},
      {.operation_id = "listDocumentationGovernance", .method = HttpMethod::Get, .path = "/docs/governance", .internal = true},
      {.operation_id = "planDocumentationChange", .method = HttpMethod::Post, .path = "/docs/change-plan", .internal = true},
      {.operation_id = "listEcosystemRepositories", .method = HttpMethod::Get, .path = "/ecosystem/repositories", .internal = true},
      {.operation_id = "planEcosystemRelease", .method = HttpMethod::Post, .path = "/ecosystem/releases/plan", .internal = true},
      {.operation_id = "createRecoverySnapshot", .method = HttpMethod::Post, .path = "/ops/recovery/snapshots", .internal = true},
      {.operation_id = "listRecoverySnapshots", .method = HttpMethod::Get, .path = "/ops/recovery/snapshots", .internal = true},
      {.operation_id = "restoreRecoverySnapshot", .method = HttpMethod::Post, .path = "/ops/recovery/snapshots/{snapshot_id}/restore", .internal = true},
      {.operation_id = "listAuditEvents", .method = HttpMethod::Get, .path = "/ops/audit-events", .internal = true},
      {.operation_id = "platformMetrics", .method = HttpMethod::Get, .path = "/ops/metrics", .internal = true},
      {.operation_id = "storageStatus", .method = HttpMethod::Get, .path = "/storage/status", .internal = true},
      {.operation_id = "exchangeExternalIdentity", .method = HttpMethod::Post, .path = "/identity/external/exchange"},
  };
}

}  // namespace spio::platform
