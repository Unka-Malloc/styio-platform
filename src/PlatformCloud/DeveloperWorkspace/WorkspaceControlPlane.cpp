#include "PlatformCloud/DeveloperWorkspace/WorkspaceControlPlaneSupport.hpp"

namespace spio::platform
{

HttpResponse
PlatformRouter::HandleSubmitJob(const HttpRequest &request) {
  if (const std::optional<std::string> error = ValidateSubmitJobRequest(request.body); error.has_value()) {
    return JsonResponse(400, FailureEnvelope("job submission rejected", *error, "ValidationError", "submitJob", 2));
  }
  std::string job_id;
  if (postgres_ != nullptr) {
    try {
      job_id = postgres_->NextJobId();
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "job submission failed", error.what(), "PostgresError", "submitJob");
    }
  }
  else {
    job_id = memory_.NextJobId();
  }
  PlatformJobRecord job = BuildQueuedJobRecord(request.body, config_, std::move(job_id));
  if (postgres_ != nullptr) {
    try {
      postgres_->SubmitJob(job);
      return JsonResponse(200, SuccessEnvelope("queued platform job", SerializeJobRecord(job)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "job submission failed", error.what(), "PostgresError", "submitJob");
    }
  }
  memory_.SubmitJob(job);
  return JsonResponse(200, SuccessEnvelope("queued platform job", SerializeJobRecord(job)));
}

HttpResponse
PlatformRouter::HandleGetJob(const RouteMatch &match) const {
  if (postgres_ != nullptr) {
    try {
      const std::optional<PlatformJobRecord> job = postgres_->GetJob(match.parameters.at("job_id"));
      if (!job.has_value()) {
        return JsonResponse(404, FailureEnvelope("job lookup failed", "job not found", "NotFound", "getJob"));
      }
      return JsonResponse(200, SuccessEnvelope("loaded platform job", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "job lookup failed", error.what(), "PostgresError", "getJob");
    }
  }
  const std::optional<PlatformJobRecord> job = memory_.GetJob(match.parameters.at("job_id"));
  if (!job.has_value()) {
    return JsonResponse(404, FailureEnvelope("job lookup failed", "job not found", "NotFound", "getJob"));
  }
  return JsonResponse(200, SuccessEnvelope("loaded platform job", SerializeJobRecord(*job)));
}

HttpResponse
PlatformRouter::HandleGetJobEvents(const RouteMatch &match) const {
  const std::string job_id = match.parameters.at("job_id");
  if (postgres_ != nullptr) {
    try {
      if (!postgres_->GetJob(job_id).has_value()) {
        return JsonResponse(404, FailureEnvelope("job event lookup failed", "job not found", "NotFound", "getJobEvents"));
      }
      nlohmann::json events = nlohmann::json::array();
      for (const JobEventRecord &event : postgres_->GetJobEvents(job_id)) {
        events.push_back(SerializeJobEvent(event));
      }
      return JsonResponse(200, SuccessEnvelope("loaded platform job events", {{"job_id", job_id}, {"events", events}}));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "job event lookup failed", error.what(), "PostgresError", "getJobEvents");
    }
  }
  if (!memory_.GetJob(job_id).has_value()) {
    return JsonResponse(404, FailureEnvelope("job event lookup failed", "job not found", "NotFound", "getJobEvents"));
  }
  nlohmann::json events = nlohmann::json::array();
  for (const JobEventRecord &event : memory_.GetJobEvents(job_id)) {
    events.push_back(SerializeJobEvent(event));
  }
  return JsonResponse(200, SuccessEnvelope("loaded platform job events", {{"job_id", job_id}, {"events", events}}));
}

HttpResponse
PlatformRouter::HandleCancelJob(const RouteMatch &match, const HttpRequest &request) {
  if (!request.body.is_object() || !request.body.contains("reason") || !request.body["reason"].is_string()) {
    return JsonResponse(400, FailureEnvelope("job cancellation failed", "reason is required", "ValidationError", "cancelJob", 2));
  }
  if (postgres_ != nullptr) {
    try {
      const std::optional<PlatformJobRecord> job =
        postgres_->CancelJob(match.parameters.at("job_id"), request.body["reason"].get<std::string>());
      if (!job.has_value()) {
        return JsonResponse(404, FailureEnvelope("job cancellation failed", "job not found or already completed", "NotFound", "cancelJob"));
      }
      return JsonResponse(200, SuccessEnvelope("cancelled platform job", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "job cancellation failed", error.what(), "PostgresError", "cancelJob");
    }
  }
  const std::optional<PlatformJobRecord> job =
    memory_.CancelJob(match.parameters.at("job_id"), request.body["reason"].get<std::string>());
  if (!job.has_value()) {
    return JsonResponse(404, FailureEnvelope("job cancellation failed", "job not found", "NotFound", "cancelJob"));
  }
  return JsonResponse(200, SuccessEnvelope("cancelled platform job", SerializeJobRecord(*job)));
}

HttpResponse
PlatformRouter::HandleRegisterWorker(const HttpRequest &request) {
  for (const std::string field : {"worker_id", "region", "worker_pool_key"}) {
    if (!request.body.contains(field) || !request.body[field].is_string()) {
      return JsonResponse(400, FailureEnvelope("worker registration rejected", field + " is required", "ValidationError", "registerWorker", 2));
    }
  }
  const int capacity = request.body.value("capacity", 0);
  if (capacity < 1) {
    return JsonResponse(400, FailureEnvelope("worker registration rejected", "capacity must be positive", "ValidationError", "registerWorker", 2));
  }
  nlohmann::json worker = {
    {"worker_id", request.body["worker_id"].get<std::string>()},
    {"region", request.body["region"].get<std::string>()},
    {"worker_pool_key", request.body["worker_pool_key"].get<std::string>()},
    {"capacity", capacity},
    {"status", "registered"},
  };
  if (postgres_ != nullptr) {
    try {
      return JsonResponse(200, SuccessEnvelope("registered platform worker", postgres_->RegisterWorker(worker)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "worker registration failed", error.what(), "PostgresError", "registerWorker");
    }
  }
  return JsonResponse(200, SuccessEnvelope("registered platform worker", memory_.RegisterWorker(worker)));
}

HttpResponse
PlatformRouter::HandleRegisterCompileContainer(const HttpRequest &request) {
  if (const std::optional<std::string> error = ValidateCompileContainerRegistration(request.body); error.has_value()) {
    return JsonResponse(400, FailureEnvelope("compile container registration rejected", *error, "ValidationError", "registerCompileContainer", 2));
  }
  const CompileContainerRecordFactory container_factory;
  CompileContainerRecord container = container_factory.CreateFromRegistration(request.body);
  if (postgres_ != nullptr) {
    try {
      return JsonResponse(200, SuccessEnvelope("registered compile container", SerializeCompileContainerRecord(postgres_->RegisterCompileContainer(container))));
    }
    catch (const PostgresStoreError &error) {
      const std::string detail = error.what();
      if (detail.find("worker is not registered") != std::string::npos) {
        return JsonResponse(403, FailureEnvelope("compile container registration rejected", "worker is not registered", "WorkerError", "registerCompileContainer"));
      }
      if (detail.find("user binding mismatch") != std::string::npos) {
        return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container user binding mismatch", "BindingError", "registerCompileContainer", 2));
      }
      if (detail.find("worker owner mismatch") != std::string::npos) {
        return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container worker owner mismatch", "BindingError", "registerCompileContainer", 2));
      }
      return FailureResponse(503, "compile container registration failed", detail, "PostgresError", "registerCompileContainer");
    }
  }

  try {
    return JsonResponse(200, SuccessEnvelope("registered compile container", SerializeCompileContainerRecord(memory_.RegisterCompileContainer(container))));
  }
  catch (const MemoryStateStoreError &error) {
    const std::string detail = error.what();
    if (detail.find("worker is not registered") != std::string::npos || detail.find("worker region or pool") != std::string::npos) {
      return JsonResponse(403, FailureEnvelope("compile container registration rejected", detail, "WorkerError", "registerCompileContainer"));
    }
    if (detail.find("user binding mismatch") != std::string::npos) {
      return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container user binding mismatch", "BindingError", "registerCompileContainer", 2));
    }
    if (detail.find("worker owner mismatch") != std::string::npos) {
      return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container worker owner mismatch", "BindingError", "registerCompileContainer", 2));
    }
    return FailureResponse(503, "compile container registration failed", detail, "MemoryStateError", "registerCompileContainer");
  }
}

HttpResponse
PlatformRouter::HandleGetCompileContainer(const RouteMatch &match) const {
  const std::string container_id = match.parameters.at("container_id");
  if (postgres_ != nullptr) {
    try {
      const std::optional<CompileContainerRecord> container = postgres_->GetCompileContainer(container_id);
      if (!container.has_value()) {
        return JsonResponse(404, FailureEnvelope("compile container lookup failed", "compile container not found", "NotFound", "getCompileContainer"));
      }
      return JsonResponse(200, SuccessEnvelope("loaded compile container", SerializeCompileContainerRecord(*container)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "compile container lookup failed", error.what(), "PostgresError", "getCompileContainer");
    }
  }
  const std::optional<CompileContainerRecord> container = memory_.GetCompileContainer(container_id);
  if (!container.has_value()) {
    return JsonResponse(404, FailureEnvelope("compile container lookup failed", "compile container not found", "NotFound", "getCompileContainer"));
  }
  return JsonResponse(200, SuccessEnvelope("loaded compile container", SerializeCompileContainerRecord(*container)));
}

HttpResponse
PlatformRouter::HandleSwitchCompileContainerWorkspace(const RouteMatch &match, const HttpRequest &request) {
  if (const std::optional<std::string> error = ValidateCompileContainerSwitch(request.body); error.has_value()) {
    return JsonResponse(400, FailureEnvelope("compile container workspace switch rejected", *error, "ValidationError", "switchCompileContainerWorkspace", 2));
  }
  const std::string container_id = match.parameters.at("container_id");
  const std::string worker_id = request.body["worker_id"].get<std::string>();
  const std::string workspace_id = request.body["workspace_id"].get<std::string>();
  const std::string reason = request.body.value("reason", "manual switch");

  if (postgres_ != nullptr) {
    try {
      const std::optional<CompileContainerRecord> current = postgres_->GetCompileContainer(container_id);
      if (!current.has_value()) {
        return JsonResponse(404, FailureEnvelope("compile container workspace switch failed", "compile container not found", "NotFound", "switchCompileContainerWorkspace"));
      }
      if (current->worker_id != worker_id) {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "worker does not own compile container", "WorkerError", "switchCompileContainerWorkspace"));
      }
      if (request.body.contains("tenant_id") && request.body["tenant_id"].get<std::string>() != current->tenant_id) {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "tenant binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
      }
      if (request.body.contains("user_id") && request.body["user_id"].get<std::string>() != current->user_id) {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "user binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
      }
      const std::optional<CompileContainerRecord> switched =
        postgres_->SwitchCompileContainerWorkspace(container_id, worker_id, workspace_id, reason);
      if (!switched.has_value()) {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "compile container is not active", "StateError", "switchCompileContainerWorkspace"));
      }
      return JsonResponse(200, SuccessEnvelope("switched compile container workspace", SerializeCompileContainerRecord(*switched)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "compile container workspace switch failed", error.what(), "PostgresError", "switchCompileContainerWorkspace");
    }
  }

  const std::optional<CompileContainerRecord> current = memory_.GetCompileContainer(container_id);
  if (!current.has_value()) {
    return JsonResponse(404, FailureEnvelope("compile container workspace switch failed", "compile container not found", "NotFound", "switchCompileContainerWorkspace"));
  }
  if (current->worker_id != worker_id) {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "worker does not own compile container", "WorkerError", "switchCompileContainerWorkspace"));
  }
  if (request.body.contains("tenant_id") && request.body["tenant_id"].get<std::string>() != current->tenant_id) {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "tenant binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
  }
  if (request.body.contains("user_id") && request.body["user_id"].get<std::string>() != current->user_id) {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "user binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
  }
  try {
    const std::optional<CompileContainerRecord> switched =
      memory_.SwitchCompileContainerWorkspace(container_id, worker_id, workspace_id, reason);
    if (!switched.has_value()) {
      return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "compile container is not active", "StateError", "switchCompileContainerWorkspace"));
    }
    return JsonResponse(200, SuccessEnvelope("switched compile container workspace", SerializeCompileContainerRecord(*switched)));
  }
  catch (const MemoryStateStoreError &error) {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", error.what(), "WorkerError", "switchCompileContainerWorkspace"));
  }
}

HttpResponse
PlatformRouter::HandleClaimJob(const HttpRequest &request) {
  for (const std::string field : {"worker_id", "region", "worker_pool_key"}) {
    if (!request.body.contains(field) || !request.body[field].is_string()) {
      return JsonResponse(400, FailureEnvelope("job claim failed", field + " is required", "ValidationError", "claimJob", 2));
    }
  }
  const std::string worker_id = request.body["worker_id"].get<std::string>();
  const std::string region = request.body["region"].get<std::string>();
  const std::string worker_pool_key = request.body["worker_pool_key"].get<std::string>();
  std::optional<std::string> compile_container_id;
  if (request.body.contains("compile_container_id")) {
    if (!request.body["compile_container_id"].is_string() || request.body["compile_container_id"].get<std::string>().empty()) {
      return JsonResponse(400, FailureEnvelope("job claim failed", "compile_container_id must be a non-empty string when present", "ValidationError", "claimJob", 2));
    }
    compile_container_id = request.body["compile_container_id"].get<std::string>();
  }
  if (postgres_ != nullptr) {
    try {
      const std::optional<PlatformJobRecord> job = compile_container_id.has_value()
                                                     ? postgres_->ClaimJobForCompileContainer(worker_id, region, worker_pool_key, *compile_container_id)
                                                     : postgres_->ClaimJob(worker_id, region, worker_pool_key);
      if (!job.has_value()) {
        return JsonResponse(200, SuccessEnvelope("no platform job available", {{"claimed", false}}));
      }
      nlohmann::json payload = {{"claimed", true}, {"job", SerializeJobRecord(*job)}};
      if (compile_container_id.has_value()) {
        if (const std::optional<CompileContainerRecord> container = postgres_->GetCompileContainer(*compile_container_id); container.has_value()) {
          payload["compile_container"] = SerializeCompileContainerRecord(*container);
        }
      }
      return JsonResponse(200, SuccessEnvelope("claimed platform job", std::move(payload)));
    }
    catch (const PostgresStoreError &error) {
      const std::string detail = error.what();
      if (detail.find("worker is not registered") != std::string::npos) {
        return JsonResponse(403, FailureEnvelope("job claim failed", "worker is not registered", "WorkerError", "claimJob"));
      }
      if (detail.find("compile container is not registered") != std::string::npos) {
        return JsonResponse(403, FailureEnvelope("job claim failed", "compile container is not registered", "WorkerError", "claimJob"));
      }
      return FailureResponse(503, "job claim failed", detail, "PostgresError", "claimJob");
    }
  }
  try {
    const std::optional<PlatformJobRecord> job = compile_container_id.has_value()
                                                   ? memory_.ClaimJobForCompileContainer(worker_id, region, worker_pool_key, *compile_container_id)
                                                   : memory_.ClaimJob(worker_id, region, worker_pool_key);
    if (!job.has_value()) {
      return JsonResponse(200, SuccessEnvelope("no platform job available", {{"claimed", false}}));
    }
    nlohmann::json payload = {{"claimed", true}, {"job", SerializeJobRecord(*job)}};
    if (compile_container_id.has_value()) {
      if (const std::optional<CompileContainerRecord> container = memory_.GetCompileContainer(*compile_container_id); container.has_value()) {
        payload["compile_container"] = SerializeCompileContainerRecord(*container);
      }
    }
    return JsonResponse(200, SuccessEnvelope("claimed platform job", std::move(payload)));
  }
  catch (const MemoryStateStoreError &error) {
    const std::string detail = error.what();
    if (detail.find("worker is not registered") != std::string::npos) {
      return JsonResponse(403, FailureEnvelope("job claim failed", "worker is not registered", "WorkerError", "claimJob"));
    }
    if (detail.find("compile container is not registered") != std::string::npos) {
      return JsonResponse(403, FailureEnvelope("job claim failed", "compile container is not registered", "WorkerError", "claimJob"));
    }
    if (detail.find("compile container is not available") != std::string::npos) {
      return JsonResponse(403, FailureEnvelope("job claim failed", "compile container is not available for this worker", "WorkerError", "claimJob"));
    }
    return FailureResponse(503, "job claim failed", detail, "MemoryStateError", "claimJob");
  }
}

HttpResponse
PlatformRouter::HandleHeartbeatJob(const RouteMatch &match, const HttpRequest &request) {
  if (postgres_ != nullptr) {
    if (!request.body.contains("worker_id") || !request.body["worker_id"].is_string()) {
      return JsonResponse(400, FailureEnvelope("job heartbeat failed", "worker_id is required", "ValidationError", "heartbeatJob", 2));
    }
    try {
      const std::optional<PlatformJobRecord> job = postgres_->HeartbeatJob(
        match.parameters.at("job_id"),
        request.body["worker_id"].get<std::string>(),
        request.body.value("message", "worker heartbeat")
      );
      if (!job.has_value()) {
        return JsonResponse(403, FailureEnvelope("job heartbeat failed", "worker does not own job", "WorkerError", "heartbeatJob"));
      }
      return JsonResponse(200, SuccessEnvelope("recorded platform job heartbeat", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "job heartbeat failed", error.what(), "PostgresError", "heartbeatJob");
    }
  }
  const std::optional<PlatformJobRecord> existing = memory_.GetJob(match.parameters.at("job_id"));
  if (!existing.has_value()) {
    return JsonResponse(404, FailureEnvelope("job heartbeat failed", "job not found", "NotFound", "heartbeatJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != existing->worker_id) {
    return JsonResponse(403, FailureEnvelope("job heartbeat failed", "worker does not own job", "WorkerError", "heartbeatJob"));
  }
  const std::optional<PlatformJobRecord> job = memory_.HeartbeatJob(
    match.parameters.at("job_id"),
    request.body["worker_id"].get<std::string>(),
    request.body.value("message", "worker heartbeat")
  );
  return JsonResponse(200, SuccessEnvelope("recorded platform job heartbeat", SerializeJobRecord(*job)));
}

HttpResponse
PlatformRouter::HandleCompleteJob(const RouteMatch &match, const HttpRequest &request) {
  if (postgres_ != nullptr) {
    if (!request.body.contains("worker_id") || !request.body["worker_id"].is_string()) {
      return JsonResponse(400, FailureEnvelope("job completion failed", "worker_id is required", "ValidationError", "completeJob", 2));
    }
    const std::string status = request.body.value("status", "");
    if (status != "succeeded" && status != "failed" && status != "cancelled") {
      return JsonResponse(400, FailureEnvelope("job completion failed", "status must be succeeded, failed, or cancelled", "ValidationError", "completeJob", 2));
    }
    std::vector<ArtifactRecord> artifacts;
    if (request.body.contains("artifacts") && request.body["artifacts"].is_array()) {
      for (const nlohmann::json &artifact : request.body["artifacts"]) {
        artifacts.push_back({
          .artifact_id = artifact.value("artifact_id", "artifact"),
          .object_key = artifact.value("object_key", ""),
          .kind = artifact.value("kind", "artifact"),
        });
      }
    }
    try {
      const std::optional<PlatformJobRecord> job = postgres_->CompleteJob(
        match.parameters.at("job_id"),
        request.body["worker_id"].get<std::string>(),
        status,
        request.body.value("message", "job completed"),
        artifacts,
        request.body.value("result", nlohmann::json::object())
      );
      if (!job.has_value()) {
        return JsonResponse(403, FailureEnvelope("job completion failed", "worker does not own job", "WorkerError", "completeJob"));
      }
      return JsonResponse(200, SuccessEnvelope("completed platform job", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "job completion failed", error.what(), "PostgresError", "completeJob");
    }
  }
  const std::optional<PlatformJobRecord> existing = memory_.GetJob(match.parameters.at("job_id"));
  if (!existing.has_value()) {
    return JsonResponse(404, FailureEnvelope("job completion failed", "job not found", "NotFound", "completeJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != existing->worker_id) {
    return JsonResponse(403, FailureEnvelope("job completion failed", "worker does not own job", "WorkerError", "completeJob"));
  }
  const std::string status = request.body.value("status", "");
  if (status != "succeeded" && status != "failed" && status != "cancelled") {
    return JsonResponse(400, FailureEnvelope("job completion failed", "status must be succeeded, failed, or cancelled", "ValidationError", "completeJob", 2));
  }
  std::vector<ArtifactRecord> artifacts;
  if (request.body.contains("artifacts") && request.body["artifacts"].is_array()) {
    for (const nlohmann::json &artifact : request.body["artifacts"]) {
      artifacts.push_back({
        .artifact_id = artifact.value("artifact_id", "artifact"),
        .object_key = artifact.value("object_key", ""),
        .kind = artifact.value("kind", "artifact"),
      });
    }
  }
  const std::optional<PlatformJobRecord> job = memory_.CompleteJob(
    match.parameters.at("job_id"),
    request.body["worker_id"].get<std::string>(),
    status,
    request.body.value("message", "job completed"),
    artifacts,
    request.body.value("result", nlohmann::json::object())
  );
  return JsonResponse(200, SuccessEnvelope("completed platform job", SerializeJobRecord(*job)));
}

}  // namespace spio::platform
