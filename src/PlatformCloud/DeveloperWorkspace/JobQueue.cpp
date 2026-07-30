#include "PlatformCloud/DeveloperWorkspace/JobQueue.hpp"

#include "PlatformCore/SourceFetch/SourceFetch.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace pafio::platform
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
  return "default";
}

template <size_t N>
std::optional<std::string> ValidateKnownFields(
    const nlohmann::json &object,
    const std::array<std::string_view, N> &allowed,
    std::string_view path)
{
  for (const auto &[field, value] : object.items())
  {
    (void) value;
    if (std::find(allowed.begin(), allowed.end(), field) == allowed.end())
    {
      return std::string(path) + "." + field + " is not part of the v1 contract";
    }
  }
  return std::nullopt;
}

std::optional<std::string> ValidatePlatformJobRequest(const nlohmann::json &job_request)
{
  if (!job_request.contains("schema_version") || !job_request["schema_version"].is_number_integer())
  {
    return "job_request.schema_version is required";
  }
  if (job_request["schema_version"].get<int>() != 1)
  {
    return "job_request.schema_version must equal 1";
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
          pafio::GitSourcePolicyViolation(source["origin"].get<std::string>(), pafio::PublicGitSourcePolicy());
      violation.has_value())
  {
    return "job_request.source.origin " + *violation;
  }
  if (source.contains("requested_revision") &&
      (!source["requested_revision"].is_string() || source["requested_revision"].get<std::string>().empty()))
  {
    return "job_request.source.requested_revision must be a non-empty string when present";
  }
  if (const std::optional<std::string> error = ValidateKnownFields(
          job_request,
          std::array<std::string_view, 6>{"schema_version", "manifest_path", "source", "profile", "workflow", "target"},
          "job_request");
      error.has_value())
  {
    return *error;
  }
  if (const std::optional<std::string> error = ValidateKnownFields(
          source,
          std::array<std::string_view, 2>{"origin", "requested_revision"},
          "job_request.source");
      error.has_value())
  {
    return *error;
  }
  if (job_request.contains("profile") &&
      (!job_request["profile"].is_string() || job_request["profile"].get<std::string>().empty()))
  {
    return "job_request.profile must be a non-empty string when present";
  }
  for (const std::string field : {"workflow", "target"})
  {
    if (job_request.contains(field) && !job_request[field].is_object())
    {
      return "job_request." + field + " must be an object when present";
    }
  }
  if (job_request.contains("workflow"))
  {
    const nlohmann::json &workflow = job_request["workflow"];
    if (const std::optional<std::string> error = ValidateKnownFields(
            workflow,
            std::array<std::string_view, 4>{"locked", "offline", "frozen", "dry_run"},
            "job_request.workflow");
        error.has_value())
    {
      return *error;
    }
    for (const std::string field : {"locked", "offline", "frozen", "dry_run"})
    {
      if (workflow.contains(field) && !workflow[field].is_boolean())
      {
        return "job_request.workflow." + field + " must be a boolean when present";
      }
    }
  }
  if (job_request.contains("target"))
  {
    const nlohmann::json &target = job_request["target"];
    if (const std::optional<std::string> error = ValidateKnownFields(
            target,
            std::array<std::string_view, 4>{"package", "bin", "test", "lib"},
            "job_request.target");
        error.has_value())
    {
      return *error;
    }
    for (const std::string field : {"package", "bin", "test"})
    {
      if (target.contains(field) && (!target[field].is_string() || target[field].get<std::string>().empty()))
      {
        return "job_request.target." + field + " must be a non-empty string when present";
      }
    }
    if (target.contains("lib") && !target["lib"].is_boolean())
    {
      return "job_request.target.lib must be a boolean when present";
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
  if (const std::optional<std::string> error = ValidatePlatformJobRequest(request["job_request"]); error.has_value())
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

}  // namespace pafio::platform
