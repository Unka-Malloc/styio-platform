#include "PlatformCloud/DeveloperWorkspace/JobQueue.hpp"

#include "PlatformCore/SourceFetch/SourceFetch.hpp"

#include <string_view>
#include <utility>

namespace spio::platform
{

namespace
{

bool IsAction(std::string_view value)
{
  return value == "build" || value == "run" || value == "test";
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
  if (const std::optional<std::string> violation =
          spio::GitSourcePolicyViolation(source["origin"].get<std::string>(), spio::PublicGitSourcePolicy());
      violation.has_value())
  {
    return "job_request.source.origin " + *violation;
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
  for (const std::string field : {"tenant_id", "user_id", "workspace_id", "action"})
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

PlatformJobRecord BuildQueuedJobRecord(const nlohmann::json &request, const PlatformConfig &config, std::string job_id)
{
  return {
      .job_id = std::move(job_id),
      .tenant_id = request["tenant_id"].get<std::string>(),
      .user_id = request["user_id"].get<std::string>(),
      .workspace_id = request["workspace_id"].get<std::string>(),
      .action = request["action"].get<std::string>(),
      .status = "queued",
      .region = request.value("region", config.region),
      .worker_pool_key = WorkerPoolFromRequest(request),
      .job_request = request["job_request"],
  };
}

}  // namespace spio::platform
