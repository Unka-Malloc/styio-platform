#include "PlatformService/JobQueue.hpp"

#include <functional>
#include <sstream>

namespace spio::platform
{

namespace
{

bool IsAction(std::string_view value)
{
  return value == "build" || value == "run" || value == "test";
}

std::string StableJobId(const nlohmann::json &request)
{
  const std::string seed =
      request.value("tenant_id", "") + "/" + request.value("workspace_id", "") + "/" + request.value("action", "");
  const size_t hash = std::hash<std::string>{}(seed);
  std::ostringstream stream;
  stream << "job-" << std::hex << hash;
  return stream.str();
}

std::string WorkerPoolFromRequest(const nlohmann::json &request)
{
  if (request.contains("preferred_worker_pool") && request["preferred_worker_pool"].is_string())
  {
    return request["preferred_worker_pool"].get<std::string>();
  }
  if (request.contains("job_request") && request["job_request"].is_object())
  {
    const nlohmann::json &job_request = request["job_request"];
    if (job_request.contains("cloud") && job_request["cloud"].is_object())
    {
      const nlohmann::json &cloud = job_request["cloud"];
      if (cloud.contains("worker_pool_key") && cloud["worker_pool_key"].is_string())
      {
        return cloud["worker_pool_key"].get<std::string>();
      }
    }
  }
  return "linux/x86_64/binary/stable/minimal";
}

std::optional<std::string> ValidateCloudBuildRequest(const nlohmann::json &job_request)
{
  if (!job_request.contains("schema_version") || !job_request["schema_version"].is_number_integer())
  {
    return "job_request.schema_version is required";
  }
  if (!job_request.contains("manifest_path") || !job_request["manifest_path"].is_string() ||
      job_request["manifest_path"].get<std::string>().empty())
  {
    return "job_request.manifest_path is required";
  }
  if (!job_request.contains("source") || !job_request["source"].is_object())
  {
    return "job_request.source is required";
  }
  const nlohmann::json &source = job_request["source"];
  if (!source.contains("origin") || !source["origin"].is_string() || source["origin"].get<std::string>().empty())
  {
    return "job_request.source.origin is required";
  }
  if (source.contains("requested_revision") &&
      (!source["requested_revision"].is_string() || source["requested_revision"].get<std::string>().empty()))
  {
    return "job_request.source.requested_revision must be a non-empty string when present";
  }
  for (const std::string field : {"toolchain", "workflow", "target", "cloud"})
  {
    if (job_request.contains(field) && !job_request[field].is_object())
    {
      return "job_request." + field + " must be an object when present";
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> ValidateSubmitJobRequest(const nlohmann::json &request)
{
  if (!request.is_object())
  {
    return "request body must be an object";
  }
  for (const std::string field : {"tenant_id", "workspace_id", "action"})
  {
    if (!request.contains(field) || !request[field].is_string() || request[field].get<std::string>().empty())
    {
      return field + " is required";
    }
  }
  if (!IsAction(request["action"].get<std::string>()))
  {
    return "action must be build, run, or test";
  }
  if (!request.contains("job_request") || !request["job_request"].is_object())
  {
    return "job_request is required";
  }
  if (const std::optional<std::string> error = ValidateCloudBuildRequest(request["job_request"]); error.has_value())
  {
    return *error;
  }
  return std::nullopt;
}

PlatformJobRecord BuildQueuedJobRecord(const nlohmann::json &request, const PlatformConfig &config)
{
  return {
      .job_id = StableJobId(request),
      .tenant_id = request["tenant_id"].get<std::string>(),
      .workspace_id = request["workspace_id"].get<std::string>(),
      .action = request["action"].get<std::string>(),
      .status = "queued",
      .region = request.value("region", config.region),
      .worker_pool_key = WorkerPoolFromRequest(request),
      .job_request = request["job_request"],
  };
}

nlohmann::json SerializeArtifact(const ArtifactRecord &artifact)
{
  return {
      {"artifact_id", artifact.artifact_id},
      {"object_key", artifact.object_key},
      {"kind", artifact.kind},
  };
}

nlohmann::json SerializeJobRecord(const PlatformJobRecord &job)
{
  nlohmann::json payload = {
      {"job_id", job.job_id},
      {"tenant_id", job.tenant_id},
      {"workspace_id", job.workspace_id},
      {"action", job.action},
      {"status", job.status},
      {"region", job.region},
      {"worker_pool_key", job.worker_pool_key},
      {"created_at", job.created_at},
  };
  if (!job.worker_id.empty())
  {
    payload["worker_id"] = job.worker_id;
  }
  if (!job.finished_at.empty())
  {
    payload["finished_at"] = job.finished_at;
  }
  if (!job.job_request.empty())
  {
    payload["job_request"] = job.job_request;
  }
  if (!job.artifacts.empty())
  {
    payload["artifacts"] = nlohmann::json::array();
    for (const ArtifactRecord &artifact : job.artifacts)
    {
      payload["artifacts"].push_back(SerializeArtifact(artifact));
    }
  }
  return payload;
}

nlohmann::json SerializeJobEvent(const JobEventRecord &event)
{
  return {
      {"event_id", event.event_id},
      {"job_id", event.job_id},
      {"status", event.status},
      {"message", event.message},
      {"created_at", event.created_at},
  };
}

}  // namespace spio::platform
