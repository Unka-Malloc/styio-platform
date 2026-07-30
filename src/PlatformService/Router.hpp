#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "PlatformCloud/DeveloperWorkspace/JobQueue.hpp"
#include "PlatformCore/Config.hpp"
#include "PlatformService/Http.hpp"
#include "PlatformService/PlatformOps/OpsManager.hpp"
#include "PlatformStorage/PlatformPersistence/MemoryStateStore.hpp"
#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"

namespace pafio::platform
{

// Routes control-plane HTTP requests to capability handlers while keeping
// identity checks, rate limiting, metrics, and state-store ownership in one
// process boundary.
class PlatformRouter
{
public:
  explicit PlatformRouter(PlatformConfig config);

  const PlatformConfig &config() const {
    return config_;
  }

  const std::vector<RouteSpec> &routes() const {
    return routes_;
  }

  // Dispatches a parsed request and returns the response envelope required by
  // the platform and registry control-plane contracts.
  HttpResponse Dispatch(const HttpRequest &request);

private:
  // Capability handlers stay declared here because they share the router-owned
  // stores; implementations live beside the capability they mutate.
  HttpResponse DispatchMatchedOperation(const RouteMatch &match, const HttpRequest &request);
  std::optional<HttpResponse> DispatchPlatformNodeOperation(const RouteMatch &match, const HttpRequest &request);
  std::optional<HttpResponse> DispatchWorkspaceOperation(const RouteMatch &match, const HttpRequest &request);
  std::optional<HttpResponse> DispatchGovernanceOperation(const RouteMatch &match, const HttpRequest &request);
  std::optional<HttpResponse> DispatchRegistryOperation(const RouteMatch &match, const HttpRequest &request);
  std::optional<HttpResponse> DispatchOperationsOperation(const RouteMatch &match, const HttpRequest &request);
  HttpResponse RequireIdentity(const RouteMatch &match, const HttpRequest &request) const;
  HttpResponse HandleHealth() const;
  HttpResponse HandleNodeSelf() const;
  HttpResponse HandleSubmitJob(const HttpRequest &request);
  HttpResponse HandleGetJob(const RouteMatch &match) const;
  HttpResponse HandleGetJobEvents(const RouteMatch &match) const;
  HttpResponse HandleCancelJob(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleRegisterWorker(const HttpRequest &request);
  HttpResponse HandleRegisterCompileContainer(const HttpRequest &request);
  HttpResponse HandleGetCompileContainer(const RouteMatch &match) const;
  HttpResponse HandleSwitchCompileContainerWorkspace(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleClaimJob(const HttpRequest &request);
  HttpResponse HandleHeartbeatJob(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleCompleteJob(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleRegisterWorkgroupCluster(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleListWorkgroupClusters(const RouteMatch &match) const;
  HttpResponse HandleMirrorStatus(const RouteMatch &match) const;
  HttpResponse HandleListDocumentationGovernance() const;
  HttpResponse HandlePlanDocumentationChange(const HttpRequest &request) const;
  HttpResponse HandleListEcosystemRepositories() const;
  HttpResponse HandlePlanEcosystemRelease(const HttpRequest &request) const;
  HttpResponse HandleRegistryStatus() const;
  HttpResponse HandleRegistryDescriptor() const;
  HttpResponse HandlePublishRelease(const HttpRequest &request);
  HttpResponse HandleVerifyRegistry(const HttpRequest &request);
  HttpResponse HandleGetPackage(const RouteMatch &match) const;
  HttpResponse HandleListPackageReleases(const RouteMatch &match) const;
  HttpResponse HandleGetPackageRelease(const RouteMatch &match) const;
  HttpResponse HandleSetPackageReleaseYanked(const RouteMatch &match, const HttpRequest &request, bool yanked);
  HttpResponse HandleListPackageOwners(const RouteMatch &match) const;
  HttpResponse HandleAddPackageOwner(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleRemovePackageOwner(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleCreatePublishToken(const HttpRequest &request);
  HttpResponse HandleListPublishTokens(const HttpRequest &request) const;
  HttpResponse HandleRevokePublishToken(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleListRepositories() const;
  HttpResponse HandleListRepositoryVersions(const RouteMatch &match) const;
  HttpResponse HandleGetPublication(const RouteMatch &match) const;
  HttpResponse HandleVerifyPublication(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleListDistributions() const;
  HttpResponse HandlePromoteDistribution(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleRollbackDistribution(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleCreateRecoverySnapshot(const HttpRequest &request);
  HttpResponse HandleListRecoverySnapshots() const;
  HttpResponse HandleRestoreRecoverySnapshot(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleListAuditEvents() const;
  HttpResponse HandleMetrics() const;
  HttpResponse HandleStorageStatus() const;
  HttpResponse HandleListReleaseChannels() const;
  HttpResponse HandleRolloutReleaseChannel(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleExchangeExternalIdentity(const HttpRequest &request) const;
  bool RegistryWriteAuthorized(const HttpRequest &request, std::string_view scope, const std::string &package_id) const;
  void RecordRegistryAudit(
    const HttpRequest &request,
    const std::string &operation,
    const nlohmann::json &target,
    const std::string &result
  );
  nlohmann::json CreateRegistryPublication(
    const std::string &change_kind,
    const std::string &change_ref,
    const std::string &generated_at
  );
  void RecordMirrorState(
    std::string freshness,
    std::string replay_cursor,
    std::string publication_id = {},
    std::string repository_version_id = {},
    std::string synced_at = {},
    int tree_size = 0
  );

  PlatformConfig config_;
  std::vector<RouteSpec> routes_;
  MemoryStateStore memory_;
  std::unique_ptr<PostgresStore> postgres_;
  PlatformRateLimiter rate_limiter_;
  PlatformRequestMetrics metrics_;
};

}  // namespace pafio::platform
