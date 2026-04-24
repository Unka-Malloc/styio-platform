#pragma once

#include "PlatformService/Config.hpp"
#include "PlatformService/Http.hpp"
#include "PlatformService/JobQueue.hpp"

#include <map>
#include <string>
#include <vector>

namespace spio::platform
{

struct RegistryMirrorState
{
  std::string mirror_id;
  std::string origin;
  std::string freshness = "lagging";
  std::string replay_cursor = "checkpoint-0000";
};

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
  HttpResponse HandleClaimJob(const HttpRequest &request);
  HttpResponse HandleHeartbeatJob(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleCompleteJob(const RouteMatch &match, const HttpRequest &request);
  HttpResponse HandleMirrorStatus(const RouteMatch &match) const;
  HttpResponse HandleRegistryStatus() const;
  HttpResponse HandlePublishRelease(const HttpRequest &request);
  HttpResponse HandleVerifyRegistry(const HttpRequest &request);
  void RecordMirrorState(std::string freshness, std::string replay_cursor);

  PlatformConfig config_;
  std::vector<RouteSpec> routes_;
  std::map<std::string, PlatformJobRecord> jobs_;
  std::map<std::string, std::vector<JobEventRecord>> events_;
  std::map<std::string, nlohmann::json> workers_;
  std::map<std::string, RegistryMirrorState> mirrors_;
  std::map<std::string, nlohmann::json> published_releases_;
};

std::vector<RouteSpec> BuildPlatformControlPlaneRoutes();
std::vector<RouteSpec> BuildRegistryControlPlaneRoutes();

}  // namespace spio::platform
