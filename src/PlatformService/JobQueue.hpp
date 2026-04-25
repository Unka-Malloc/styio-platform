#pragma once

#include "PlatformService/Config.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace spio::platform
{

struct ArtifactRecord
{
  std::string artifact_id;
  std::string object_key;
  std::string kind;
};

struct PlatformJobRecord
{
  std::string job_id;
  std::string tenant_id;
  std::string workspace_id;
  std::string action;
  std::string status = "queued";
  std::string region;
  std::string worker_pool_key;
  std::string created_at = "2026-04-24T00:00:00Z";
  std::string worker_id;
  std::string finished_at;
  std::vector<ArtifactRecord> artifacts;
};

struct JobEventRecord
{
  std::string event_id;
  std::string job_id;
  std::string status;
  std::string message;
  std::string created_at = "2026-04-24T00:00:00Z";
};

std::optional<std::string> ValidateSubmitJobRequest(const nlohmann::json &request);
PlatformJobRecord BuildQueuedJobRecord(const nlohmann::json &request, const PlatformConfig &config);
nlohmann::json SerializeArtifact(const ArtifactRecord &artifact);
nlohmann::json SerializeJobRecord(const PlatformJobRecord &job);
nlohmann::json SerializeJobEvent(const JobEventRecord &event);

}  // namespace spio::platform
