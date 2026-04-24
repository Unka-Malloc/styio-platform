#include "PlatformService/Router.hpp"

#include "PlatformService/ObjectStore.hpp"
#include "PlatformService/PostgresStore.hpp"

namespace spio::platform
{

namespace
{

bool IsInternalRole(const MtlsIdentity &identity)
{
  return identity.role == "worker" || identity.role == "mirror" || identity.role == "registry-writer" || identity.role == "operator";
}

HttpResponse JsonResponse(int status, nlohmann::json body)
{
  return {.status_code = status, .body = std::move(body)};
}

}  // namespace

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
      {.operation_id = "claimJob", .method = HttpMethod::Post, .path = "/jobs/claim", .internal = true},
      {.operation_id = "heartbeatJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/heartbeat", .internal = true},
      {.operation_id = "completeJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/complete", .internal = true},
      {.operation_id = "mirrorStatus", .method = HttpMethod::Get, .path = "/mirrors/{mirror_id}/status"},
  };
}

PlatformRouter::PlatformRouter(PlatformConfig config)
    : config_(std::move(config)), routes_(BuildPlatformControlPlaneRoutes())
{
}

HttpResponse PlatformRouter::Dispatch(const HttpRequest &request)
{
  const std::optional<RouteMatch> match = MatchRoute(routes_, request.method, request.path);
  if (!match.has_value())
  {
    return JsonResponse(404, FailureEnvelope("route not found", request.path, "NotFound", "unknown", 17));
  }
  if (const HttpResponse identity_response = RequireIdentity(*match, request); identity_response.status_code != 200)
  {
    return identity_response;
  }

  const std::string &operation = match->route.operation_id;
  if (operation == "health")
  {
    return HandleHealth();
  }
  if (operation == "nodeSelf")
  {
    return HandleNodeSelf();
  }
  if (operation == "submitJob")
  {
    return HandleSubmitJob(request);
  }
  if (operation == "getJob")
  {
    return HandleGetJob(*match);
  }
  if (operation == "getJobEvents")
  {
    return HandleGetJobEvents(*match);
  }
  if (operation == "cancelJob")
  {
    return HandleCancelJob(*match, request);
  }
  if (operation == "registerWorker")
  {
    return HandleRegisterWorker(request);
  }
  if (operation == "claimJob")
  {
    return HandleClaimJob(request);
  }
  if (operation == "heartbeatJob")
  {
    return HandleHeartbeatJob(*match, request);
  }
  if (operation == "completeJob")
  {
    return HandleCompleteJob(*match, request);
  }
  if (operation == "mirrorStatus")
  {
    return HandleMirrorStatus(*match);
  }
  return JsonResponse(500, FailureEnvelope("route handler missing", operation, "InternalError", operation));
}

HttpResponse PlatformRouter::RequireIdentity(const RouteMatch &match, const HttpRequest &request) const
{
  if (!config_.mtls.required)
  {
    return JsonResponse(200, {});
  }
  if (!request.identity.has_value())
  {
    return JsonResponse(401, FailureEnvelope("mTLS identity is required", "missing client certificate identity", "AuthError", match.route.operation_id, 2));
  }
  if (match.route.internal && !IsInternalRole(*request.identity))
  {
    return JsonResponse(403, FailureEnvelope("mTLS identity is not authorized", "internal route requires a service role", "AuthError", match.route.operation_id, 2));
  }
  return JsonResponse(200, {});
}

HttpResponse PlatformRouter::HandleHealth() const
{
  const bool ready = !config_.postgres_dsn.empty() && IsObjectStoreProviderImplemented(ParseObjectStoreProvider(config_.object_store.provider));
  nlohmann::json payload = {
      {"service", "styio-platformd"},
      {"status", ready ? "ready" : "degraded"},
      {"region", config_.region},
      {"node_id", config_.node_id},
      {"contract_version", "v1"},
      {"roles", config_.roles},
  };
  return JsonResponse(200, SuccessEnvelope(ready ? "styio-platform is ready" : "styio-platform is degraded", payload));
}

HttpResponse PlatformRouter::HandleNodeSelf() const
{
  nlohmann::json payload = {
      {"node_id", config_.node_id},
      {"region", config_.region},
      {"roles", config_.roles},
      {"postgres_configured", LooksLikePostgresDsn(config_.postgres_dsn)},
      {"object_store_provider", ToString(ParseObjectStoreProvider(config_.object_store.provider))},
      {"mtls_required", config_.mtls.required},
  };
  return JsonResponse(200, SuccessEnvelope("resolved current platform node", payload));
}

HttpResponse PlatformRouter::HandleSubmitJob(const HttpRequest &request)
{
  if (const std::optional<std::string> error = ValidateSubmitJobRequest(request.body); error.has_value())
  {
    return JsonResponse(400, FailureEnvelope("job submission rejected", *error, "ValidationError", "submitJob", 2));
  }
  PlatformJobRecord job = BuildQueuedJobRecord(request.body, config_);
  jobs_[job.job_id] = job;
  events_[job.job_id].push_back({
      .event_id = "event-queued",
      .job_id = job.job_id,
      .status = "queued",
      .message = "job queued",
  });
  return JsonResponse(200, SuccessEnvelope("queued platform job", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleGetJob(const RouteMatch &match) const
{
  const auto job = jobs_.find(match.parameters.at("job_id"));
  if (job == jobs_.end())
  {
    return JsonResponse(404, FailureEnvelope("job lookup failed", "job not found", "NotFound", "getJob"));
  }
  return JsonResponse(200, SuccessEnvelope("loaded platform job", SerializeJobRecord(job->second)));
}

HttpResponse PlatformRouter::HandleGetJobEvents(const RouteMatch &match) const
{
  const std::string job_id = match.parameters.at("job_id");
  const auto found = events_.find(job_id);
  if (found == events_.end())
  {
    return JsonResponse(404, FailureEnvelope("job event lookup failed", "job not found", "NotFound", "getJobEvents"));
  }
  nlohmann::json events = nlohmann::json::array();
  for (const JobEventRecord &event : found->second)
  {
    events.push_back(SerializeJobEvent(event));
  }
  return JsonResponse(200, SuccessEnvelope("loaded platform job events", {{"job_id", job_id}, {"events", events}}));
}

HttpResponse PlatformRouter::HandleCancelJob(const RouteMatch &match, const HttpRequest &request)
{
  if (!request.body.is_object() || !request.body.contains("reason") || !request.body["reason"].is_string())
  {
    return JsonResponse(400, FailureEnvelope("job cancellation failed", "reason is required", "ValidationError", "cancelJob", 2));
  }
  PlatformJobRecord &job = jobs_[match.parameters.at("job_id")];
  if (job.job_id.empty())
  {
    return JsonResponse(404, FailureEnvelope("job cancellation failed", "job not found", "NotFound", "cancelJob"));
  }
  job.status = "cancelled";
  job.finished_at = "2026-04-24T00:01:00Z";
  events_[job.job_id].push_back({
      .event_id = "event-cancelled",
      .job_id = job.job_id,
      .status = "cancelled",
      .message = request.body["reason"].get<std::string>(),
      .created_at = job.finished_at,
  });
  return JsonResponse(200, SuccessEnvelope("cancelled platform job", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleRegisterWorker(const HttpRequest &request)
{
  for (const std::string field : {"worker_id", "region", "worker_pool_key"})
  {
    if (!request.body.contains(field) || !request.body[field].is_string())
    {
      return JsonResponse(400, FailureEnvelope("worker registration rejected", field + " is required", "ValidationError", "registerWorker", 2));
    }
  }
  const int capacity = request.body.value("capacity", 0);
  if (capacity < 1)
  {
    return JsonResponse(400, FailureEnvelope("worker registration rejected", "capacity must be positive", "ValidationError", "registerWorker", 2));
  }
  nlohmann::json worker = {
      {"worker_id", request.body["worker_id"].get<std::string>()},
      {"region", request.body["region"].get<std::string>()},
      {"worker_pool_key", request.body["worker_pool_key"].get<std::string>()},
      {"status", "registered"},
  };
  workers_[worker["worker_id"].get<std::string>()] = worker;
  return JsonResponse(200, SuccessEnvelope("registered platform worker", worker));
}

HttpResponse PlatformRouter::HandleClaimJob(const HttpRequest &request)
{
  for (const std::string field : {"worker_id", "region", "worker_pool_key"})
  {
    if (!request.body.contains(field) || !request.body[field].is_string())
    {
      return JsonResponse(400, FailureEnvelope("job claim failed", field + " is required", "ValidationError", "claimJob", 2));
    }
  }
  const std::string worker_id = request.body["worker_id"].get<std::string>();
  if (!workers_.contains(worker_id))
  {
    return JsonResponse(403, FailureEnvelope("job claim failed", "worker is not registered", "WorkerError", "claimJob"));
  }
  const std::string region = request.body["region"].get<std::string>();
  const std::string worker_pool_key = request.body["worker_pool_key"].get<std::string>();
  for (auto &[job_id, job] : jobs_)
  {
    if (job.status == "queued" && job.region == region && job.worker_pool_key == worker_pool_key)
    {
      job.status = "running";
      job.worker_id = worker_id;
      events_[job_id].push_back({
          .event_id = "event-running",
          .job_id = job_id,
          .status = "running",
          .message = "job claimed by worker",
      });
      return JsonResponse(200, SuccessEnvelope("claimed platform job", {{"claimed", true}, {"job", SerializeJobRecord(job)}}));
    }
  }
  return JsonResponse(200, SuccessEnvelope("no platform job available", {{"claimed", false}}));
}

HttpResponse PlatformRouter::HandleHeartbeatJob(const RouteMatch &match, const HttpRequest &request)
{
  PlatformJobRecord &job = jobs_[match.parameters.at("job_id")];
  if (job.job_id.empty())
  {
    return JsonResponse(404, FailureEnvelope("job heartbeat failed", "job not found", "NotFound", "heartbeatJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != job.worker_id)
  {
    return JsonResponse(403, FailureEnvelope("job heartbeat failed", "worker does not own job", "WorkerError", "heartbeatJob"));
  }
  events_[job.job_id].push_back({
      .event_id = "event-heartbeat",
      .job_id = job.job_id,
      .status = job.status,
      .message = request.body.value("message", "worker heartbeat"),
  });
  return JsonResponse(200, SuccessEnvelope("recorded platform job heartbeat", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleCompleteJob(const RouteMatch &match, const HttpRequest &request)
{
  PlatformJobRecord &job = jobs_[match.parameters.at("job_id")];
  if (job.job_id.empty())
  {
    return JsonResponse(404, FailureEnvelope("job completion failed", "job not found", "NotFound", "completeJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != job.worker_id)
  {
    return JsonResponse(403, FailureEnvelope("job completion failed", "worker does not own job", "WorkerError", "completeJob"));
  }
  const std::string status = request.body.value("status", "");
  if (status != "succeeded" && status != "failed" && status != "cancelled")
  {
    return JsonResponse(400, FailureEnvelope("job completion failed", "status must be succeeded, failed, or cancelled", "ValidationError", "completeJob", 2));
  }
  job.status = status;
  job.finished_at = "2026-04-24T00:02:00Z";
  if (request.body.contains("artifacts") && request.body["artifacts"].is_array())
  {
    for (const nlohmann::json &artifact : request.body["artifacts"])
    {
      job.artifacts.push_back({
          .artifact_id = artifact.value("artifact_id", "artifact"),
          .object_key = artifact.value("object_key", ""),
          .kind = artifact.value("kind", "artifact"),
      });
    }
  }
  events_[job.job_id].push_back({
      .event_id = "event-completed",
      .job_id = job.job_id,
      .status = job.status,
      .message = request.body.value("message", "job completed"),
      .created_at = job.finished_at,
  });
  return JsonResponse(200, SuccessEnvelope("completed platform job", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleMirrorStatus(const RouteMatch &match) const
{
  nlohmann::json payload = {
      {"mirror_id", match.parameters.at("mirror_id")},
      {"region", config_.region},
      {"origin", "registry-primary"},
      {"freshness", "unknown"},
      {"replay_cursor", "uninitialized"},
  };
  return JsonResponse(200, SuccessEnvelope("loaded mirror freshness", payload));
}

}  // namespace spio::platform
