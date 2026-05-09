#pragma once

#include "PlatformCore/Config.hpp"
#include "PlatformService/Http.hpp"
#include "PlatformCloud/DeveloperWorkspace/JobQueue.hpp"
#include "PlatformStorage/PlatformPersistence/MemoryStateStore.hpp"
#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace spio::platform
{

class PlatformRouter
{
public:
  explicit PlatformRouter(PlatformConfig config);

  const PlatformConfig &config() const { return config_; }
  const std::vector<RouteSpec> &routes() const { return routes_; }
  HttpResponse Dispatch(const HttpRequest &request);

private:
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
  bool RegistryWriteAuthorized(const HttpRequest &request, std::string_view scope, const std::string &package_id) const;
  void RecordRegistryAudit(
      const HttpRequest &request,
      const std::string &operation,
      const nlohmann::json &target,
      const std::string &result);
  nlohmann::json CreateRegistryPublication(
      const std::string &change_kind,
      const std::string &change_ref,
      const std::string &generated_at);
  void RecordMirrorState(
      std::string freshness,
      std::string replay_cursor,
      std::string publication_id = {},
      std::string repository_version_id = {},
      std::string synced_at = {},
      int tree_size = 0);

  PlatformConfig config_;
  std::vector<RouteSpec> routes_;
  MemoryStateStore memory_;
  std::unique_ptr<PostgresStore> postgres_;
};

std::vector<RouteSpec> BuildPlatformControlPlaneRoutes();
std::vector<RouteSpec> BuildRegistryControlPlaneRoutes();

}  // namespace spio::platform
