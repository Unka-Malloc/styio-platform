#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryRoutes.hpp"
#include "PlatformService/RouterSupport.hpp"
#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"

namespace spio::platform
{

PlatformRouter::PlatformRouter(PlatformConfig config) :
    config_(std::move(config)), routes_(BuildPlatformControlPlaneRoutes()) {
  const std::vector<RouteSpec> registry_routes = BuildRegistryControlPlaneRoutes();
  routes_.insert(routes_.end(), registry_routes.begin(), registry_routes.end());
  memory_.RecordMirrorState(
    config_.registry.mirror_id,
    config_.region,
    config_.registry.mirror_origin,
    "lagging",
    "checkpoint-0000"
  );
  if (UsesPostgresState(config_)) {
    postgres_ = std::make_unique<PostgresStore>(config_.postgres_dsn);
  }
}

HttpResponse
PlatformRouter::Dispatch(const HttpRequest &request) {
  const std::optional<RouteMatch> match = MatchRoute(routes_, request.method, request.path);
  if (!match.has_value()) {
    return JsonResponse(404, FailureEnvelope("route not found", request.path, "NotFound", "unknown", 17));
  }
  if (const HttpResponse identity_response = RequireIdentity(*match, request); identity_response.status_code != 200) {
    return identity_response;
  }

  const std::string &operation = match->route.operation_id;
  if (!rate_limiter_.Allow(RequestSubject(request), operation, 600, 60, UnixSecondsNow())) {
    metrics_.Record(operation, 429);
    return FailureResponse(429, "request rate limited", "rate limit exceeded", "RateLimitError", operation, 17);
  }
  metrics_.Record(operation, 0);
  return DispatchMatchedOperation(*match, request);
}

HttpResponse
PlatformRouter::DispatchMatchedOperation(const RouteMatch &match, const HttpRequest &request) {
  if (std::optional<HttpResponse> response = DispatchPlatformNodeOperation(match, request)) {
    return *response;
  }
  if (std::optional<HttpResponse> response = DispatchWorkspaceOperation(match, request)) {
    return *response;
  }
  if (std::optional<HttpResponse> response = DispatchGovernanceOperation(match, request)) {
    return *response;
  }
  if (std::optional<HttpResponse> response = DispatchRegistryOperation(match, request)) {
    return *response;
  }
  if (std::optional<HttpResponse> response = DispatchOperationsOperation(match, request)) {
    return *response;
  }
  const std::string &operation = match.route.operation_id;
  return JsonResponse(500, FailureEnvelope("route handler missing", operation, "InternalError", operation));
}

std::optional<HttpResponse>
PlatformRouter::DispatchPlatformNodeOperation(
  const RouteMatch &match,
  const HttpRequest &request
) {
  const std::string &operation = match.route.operation_id;
  if (operation == "health") {
    return HandleHealth();
  }
  if (operation == "nodeSelf") {
    return HandleNodeSelf();
  }
  if (operation == "registerWorkgroupCluster") {
    return HandleRegisterWorkgroupCluster(match, request);
  }
  if (operation == "listWorkgroupClusters") {
    return HandleListWorkgroupClusters(match);
  }
  return std::nullopt;
}

std::optional<HttpResponse>
PlatformRouter::DispatchWorkspaceOperation(
  const RouteMatch &match,
  const HttpRequest &request
) {
  const std::string &operation = match.route.operation_id;
  if (operation == "submitJob") {
    return HandleSubmitJob(request);
  }
  if (operation == "getJob") {
    return HandleGetJob(match);
  }
  if (operation == "getJobEvents") {
    return HandleGetJobEvents(match);
  }
  if (operation == "cancelJob") {
    return HandleCancelJob(match, request);
  }
  if (operation == "registerWorker") {
    return HandleRegisterWorker(request);
  }
  if (operation == "registerCompileContainer") {
    return HandleRegisterCompileContainer(request);
  }
  if (operation == "getCompileContainer") {
    return HandleGetCompileContainer(match);
  }
  if (operation == "switchCompileContainerWorkspace") {
    return HandleSwitchCompileContainerWorkspace(match, request);
  }
  if (operation == "claimJob") {
    return HandleClaimJob(request);
  }
  if (operation == "heartbeatJob") {
    return HandleHeartbeatJob(match, request);
  }
  if (operation == "completeJob") {
    return HandleCompleteJob(match, request);
  }
  return std::nullopt;
}

std::optional<HttpResponse>
PlatformRouter::DispatchGovernanceOperation(
  const RouteMatch &match,
  const HttpRequest &request
) {
  const std::string &operation = match.route.operation_id;
  if (operation == "listDocumentationGovernance") {
    return HandleListDocumentationGovernance();
  }
  if (operation == "planDocumentationChange") {
    return HandlePlanDocumentationChange(request);
  }
  if (operation == "listEcosystemRepositories") {
    return HandleListEcosystemRepositories();
  }
  if (operation == "planEcosystemRelease") {
    return HandlePlanEcosystemRelease(request);
  }
  return std::nullopt;
}

std::optional<HttpResponse>
PlatformRouter::DispatchRegistryOperation(
  const RouteMatch &match,
  const HttpRequest &request
) {
  const std::string &operation = match.route.operation_id;
  if (operation == "mirrorStatus") {
    return HandleMirrorStatus(match);
  }
  if (operation == "registryStatus") {
    return HandleRegistryStatus();
  }
  if (operation == "registryDescriptor") {
    return HandleRegistryDescriptor();
  }
  if (operation == "publishRelease") {
    return HandlePublishRelease(request);
  }
  if (operation == "verifyRegistry") {
    return HandleVerifyRegistry(request);
  }
  if (operation == "getPackage") {
    return HandleGetPackage(match);
  }
  if (operation == "listPackageReleases") {
    return HandleListPackageReleases(match);
  }
  if (operation == "getPackageRelease") {
    return HandleGetPackageRelease(match);
  }
  if (operation == "yankPackageRelease") {
    return HandleSetPackageReleaseYanked(match, request, true);
  }
  if (operation == "unyankPackageRelease") {
    return HandleSetPackageReleaseYanked(match, request, false);
  }
  if (operation == "listPackageOwners") {
    return HandleListPackageOwners(match);
  }
  if (operation == "addPackageOwner") {
    return HandleAddPackageOwner(match, request);
  }
  if (operation == "removePackageOwner") {
    return HandleRemovePackageOwner(match, request);
  }
  if (operation == "createPublishToken") {
    return HandleCreatePublishToken(request);
  }
  if (operation == "listPublishTokens") {
    return HandleListPublishTokens(request);
  }
  if (operation == "revokePublishToken") {
    return HandleRevokePublishToken(match, request);
  }
  if (operation == "listRepositories") {
    return HandleListRepositories();
  }
  if (operation == "listRepositoryVersions") {
    return HandleListRepositoryVersions(match);
  }
  if (operation == "getPublication") {
    return HandleGetPublication(match);
  }
  if (operation == "verifyPublication") {
    return HandleVerifyPublication(match, request);
  }
  if (operation == "listDistributions") {
    return HandleListDistributions();
  }
  if (operation == "promoteDistribution") {
    return HandlePromoteDistribution(match, request);
  }
  if (operation == "rollbackDistribution") {
    return HandleRollbackDistribution(match, request);
  }
  if (operation == "listReleaseChannels") {
    return HandleListReleaseChannels();
  }
  if (operation == "rolloutReleaseChannel") {
    return HandleRolloutReleaseChannel(match, request);
  }
  return std::nullopt;
}

std::optional<HttpResponse>
PlatformRouter::DispatchOperationsOperation(
  const RouteMatch &match,
  const HttpRequest &request
) {
  const std::string &operation = match.route.operation_id;
  if (operation == "createRecoverySnapshot") {
    return HandleCreateRecoverySnapshot(request);
  }
  if (operation == "listRecoverySnapshots") {
    return HandleListRecoverySnapshots();
  }
  if (operation == "restoreRecoverySnapshot") {
    return HandleRestoreRecoverySnapshot(match, request);
  }
  if (operation == "listAuditEvents") {
    return HandleListAuditEvents();
  }
  if (operation == "platformMetrics") {
    return HandleMetrics();
  }
  if (operation == "storageStatus") {
    return HandleStorageStatus();
  }
  if (operation == "exchangeExternalIdentity") {
    return HandleExchangeExternalIdentity(request);
  }
  return std::nullopt;
}

HttpResponse
PlatformRouter::RequireIdentity(const RouteMatch &match, const HttpRequest &request) const {
  if (match.route.operation_id == "exchangeExternalIdentity") {
    return JsonResponse(200, {});
  }
  if (!config_.mtls.required) {
    return JsonResponse(200, {});
  }
  if (!request.identity.has_value() && match.route.internal && IsRegistryTokenCapableOperation(match.route.operation_id) && RegistryTokenFromHeaders(request.headers).has_value()) {
    return JsonResponse(200, {});
  }
  if (!request.identity.has_value()) {
    return FailureResponse(
      401,
      "mTLS identity is required",
      "missing client certificate identity",
      "AuthError",
      match.route.operation_id,
      2
    );
  }
  if (match.route.internal && !IsInternalRole(*request.identity)) {
    return FailureResponse(
      403,
      "mTLS identity is not authorized",
      "internal route requires a service role",
      "AuthError",
      match.route.operation_id,
      2
    );
  }
  if (!IsAuthorizedForOperation(match.route.operation_id, *request.identity)) {
    return FailureResponse(
      403,
      "mTLS identity is not authorized",
      "identity role is not allowed for this operation",
      "AuthError",
      match.route.operation_id,
      2
    );
  }
  return JsonResponse(200, {});
}

HttpResponse
PlatformRouter::HandleHealth() const {
  const bool postgres_ready =
    !UsesPostgresState(config_) || (LooksLikePostgresDsn(config_.postgres_dsn) && PostgresDriverAvailable());
  const bool ready = postgres_ready && IsObjectStoreProviderImplemented(ParseObjectStoreProvider(config_.object_store.provider));
  nlohmann::json payload = {
    {"service", "styio-platformd"},
    {"status", ready ? "ready" : "degraded"},
    {"region", config_.region},
    {"node_id", config_.node_id},
    {"contract_version", "v1"},
    {"roles", config_.roles},
    {"state_backend", config_.state_backend},
    {"postgres_driver_available", PostgresDriverAvailable()},
  };
  return JsonResponse(200, SuccessEnvelope(ready ? "styio-platform is ready" : "styio-platform is degraded", payload));
}

HttpResponse
PlatformRouter::HandleNodeSelf() const {
  nlohmann::json payload = {
    {"node_id", config_.node_id},
    {"region", config_.region},
    {"roles", config_.roles},
    {"state_backend", config_.state_backend},
    {"postgres_configured", LooksLikePostgresDsn(config_.postgres_dsn)},
    {"postgres_driver_available", PostgresDriverAvailable()},
    {"object_store_provider", ToString(ParseObjectStoreProvider(config_.object_store.provider))},
    {"mtls_required", config_.mtls.required},
  };
  return JsonResponse(200, SuccessEnvelope("resolved current platform node", payload));
}

HttpResponse
PlatformRouter::HandleRegisterWorkgroupCluster(const RouteMatch &match, const HttpRequest &request) {
  const std::string workgroup_id = match.parameters.at("workgroup_id");
  if (!config_.workgroup.enabled) {
    return JsonResponse(403, FailureEnvelope("workgroup registration rejected", "workgroup support is disabled", "PolicyError", "registerWorkgroupCluster", 2));
  }
  if (request.identity.has_value() && request.identity->tenant_id != config_.workgroup.registration_tenant) {
    return JsonResponse(403, FailureEnvelope("workgroup registration rejected", "identity tenant is not allowed to register clusters", "AuthError", "registerWorkgroupCluster", 2));
  }
  if (!config_.workgroup.registration_token.empty() && request.body.value("registration_token", "") != config_.workgroup.registration_token) {
    return JsonResponse(403, FailureEnvelope("workgroup registration rejected", "registration token is invalid", "AuthError", "registerWorkgroupCluster", 2));
  }
  if (const std::optional<std::string> error = ValidateWorkgroupClusterRegistration(workgroup_id, request.body); error.has_value()) {
    return JsonResponse(400, FailureEnvelope("workgroup registration rejected", *error, "ValidationError", "registerWorkgroupCluster", 2));
  }

  nlohmann::json cluster = BuildWorkgroupClusterRecord(workgroup_id, request.body, config_, request.identity);
  if (postgres_ != nullptr) {
    try {
      return JsonResponse(200, SuccessEnvelope("registered workgroup cluster", postgres_->RegisterWorkgroupCluster(workgroup_id, cluster)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "workgroup registration failed", error.what(), "PostgresError", "registerWorkgroupCluster");
    }
  }
  return JsonResponse(200, SuccessEnvelope("registered workgroup cluster", memory_.RegisterWorkgroupCluster(workgroup_id, cluster)));
}

HttpResponse
PlatformRouter::HandleListWorkgroupClusters(const RouteMatch &match) const {
  const std::string workgroup_id = match.parameters.at("workgroup_id");
  if (!IsSafeWorkgroupId(workgroup_id)) {
    return JsonResponse(400, FailureEnvelope("workgroup lookup rejected", "workgroup_id is invalid", "ValidationError", "listWorkgroupClusters", 2));
  }
  nlohmann::json clusters = nlohmann::json::array();
  if (postgres_ != nullptr) {
    try {
      clusters = postgres_->ListWorkgroupClusters(workgroup_id);
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "workgroup lookup failed", error.what(), "PostgresError", "listWorkgroupClusters");
    }
  }
  else {
    clusters = memory_.ListWorkgroupClusters(workgroup_id);
  }
  return JsonResponse(
    200,
    SuccessEnvelope(
      "loaded workgroup clusters",
      {
        {"workgroup_id", workgroup_id},
        {"policy", WorkgroupPolicyPayload(config_)},
        {"clusters", clusters},
      }
    )
  );
}

}  // namespace spio::platform
