#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pafio::platform
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
  std::string user_id;
  std::string workspace_id;
  std::string action;
  std::string status = "queued";
  std::string region;
  std::string worker_pool_key;
  std::string created_at = "2026-04-24T00:00:00Z";
  std::string worker_id;
  std::string finished_at;
  nlohmann::json job_request = nlohmann::json::object();
  std::vector<ArtifactRecord> artifacts;
};

struct CompileContainerRecord
{
  std::string container_id;
  std::string worker_id;
  std::string tenant_id;
  std::string user_id;
  std::string current_workspace_id;
  std::string region;
  std::string worker_pool_key;
  std::string status = "active";
  int capacity = 1;
  int workspace_generation = 1;
  std::string created_at = "2026-04-24T00:00:00Z";
  std::string last_switched_at;
  std::string last_switch_reason;
};

struct JobEventRecord
{
  std::string event_id;
  std::string job_id;
  std::string status;
  std::string message;
  std::string created_at = "2026-04-24T00:00:00Z";
};

struct MirrorCursorRecord
{
  std::string mirror_id;
  std::string region;
  std::string origin;
  std::string freshness;
  std::string replay_cursor;
  std::string publication_id;
  std::string repository_version_id;
  std::string synced_at;
  int tree_size = 0;
};

struct RegistryPackageRecord
{
  std::string package_id;
  std::string package_namespace;
  std::string name;
  std::string created_at;
  std::string created_by;
  std::string visibility = "public";
};

struct RegistryPackageReleaseRecord
{
  std::string package_id;
  std::string version;
  std::string edition = "2026";
  std::string manifest_sha256;
  std::string source_artifact_sha256;
  nlohmann::json dependencies = nlohmann::json::array();
  std::string publisher_id;
  std::string published_at;
  bool yanked = false;
  std::string yanked_reason;
};

struct RegistryPackageOwnerRecord
{
  std::string package_id;
  std::string owner_id;
  std::string owner_kind = "user";
  std::string role = "owner";
  std::string added_by;
  std::string added_at;
};

struct RegistryPublishTokenRecord
{
  std::string token_id;
  std::string token_hash;
  std::string owner_id;
  std::vector<std::string> scopes;
  std::vector<std::string> package_patterns;
  std::string expires_at;
  std::string revoked_at;
  std::string created_at;
};

struct RegistryRepositoryRecord
{
  std::string repository_id;
  std::string name;
  std::string tenant_id;
  nlohmann::json policy = nlohmann::json::object();
  std::string created_at;
};

struct RegistryRepositoryVersionRecord
{
  std::string repository_version_id;
  std::string repository_id;
  int sequence = 0;
  std::string change_kind;
  std::string change_ref;
  std::string created_at;
};

struct RegistryPublicationRecord
{
  std::string publication_id;
  std::string repository_version_id;
  int layout_version = 2;
  std::string root_path;
  std::string manifest_sha256;
  int tree_size = 0;
  std::string created_at;
  bool verified = false;
};

struct RegistryDistributionRecord
{
  std::string distribution_id;
  std::string repository_id;
  std::string name;
  std::string base_url;
  std::string current_publication_id;
  std::string previous_publication_id;
  std::string updated_at;
};

struct RegistryAuditEventRecord
{
  std::string event_id;
  std::string actor_id;
  std::string operation;
  nlohmann::json target = nlohmann::json::object();
  std::string request_id;
  std::string result;
  std::string created_at;
};

nlohmann::json SerializeArtifact(const ArtifactRecord &artifact);
nlohmann::json SerializeJobRecord(const PlatformJobRecord &job);
nlohmann::json SerializeJobEvent(const JobEventRecord &event);
nlohmann::json SerializeCompileContainerRecord(const CompileContainerRecord &container);
nlohmann::json SerializeMirrorCursorRecord(const MirrorCursorRecord &mirror);
nlohmann::json SerializeRegistryPackageRecord(const RegistryPackageRecord &record);
nlohmann::json SerializeRegistryPackageReleaseRecord(const RegistryPackageReleaseRecord &record);
nlohmann::json SerializeRegistryPackageOwnerRecord(const RegistryPackageOwnerRecord &record);
nlohmann::json SerializeRegistryPublishTokenRecord(const RegistryPublishTokenRecord &record, bool include_hash = false);
nlohmann::json SerializeRegistryRepositoryRecord(const RegistryRepositoryRecord &record);
nlohmann::json SerializeRegistryRepositoryVersionRecord(const RegistryRepositoryVersionRecord &record);
nlohmann::json SerializeRegistryPublicationRecord(const RegistryPublicationRecord &record);
nlohmann::json SerializeRegistryDistributionRecord(const RegistryDistributionRecord &record);
nlohmann::json SerializeRegistryAuditEventRecord(const RegistryAuditEventRecord &record);

class CompileContainerRecordFactory
{
public:
  CompileContainerRecord CreateFromRegistration(const nlohmann::json &body) const;
};

}  // namespace pafio::platform
