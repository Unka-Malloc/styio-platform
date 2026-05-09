#include "PlatformStorage/PlatformPersistence/StateRecords.hpp"

namespace spio::platform
{

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
      {"user_id", job.user_id},
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

nlohmann::json SerializeCompileContainerRecord(const CompileContainerRecord &container)
{
  nlohmann::json payload = {
      {"container_id", container.container_id},
      {"worker_id", container.worker_id},
      {"tenant_id", container.tenant_id},
      {"user_id", container.user_id},
      {"current_workspace_id", container.current_workspace_id},
      {"region", container.region},
      {"worker_pool_key", container.worker_pool_key},
      {"status", container.status},
      {"capacity", container.capacity},
      {"workspace_generation", container.workspace_generation},
      {"created_at", container.created_at},
  };
  if (!container.last_switched_at.empty())
  {
    payload["last_switched_at"] = container.last_switched_at;
  }
  if (!container.last_switch_reason.empty())
  {
    payload["last_switch_reason"] = container.last_switch_reason;
  }
  return payload;
}

nlohmann::json SerializeMirrorCursorRecord(const MirrorCursorRecord &mirror)
{
  nlohmann::json payload = {
      {"mirror_id", mirror.mirror_id},
      {"region", mirror.region},
      {"origin", mirror.origin},
      {"freshness", mirror.freshness},
      {"replay_cursor", mirror.replay_cursor},
  };
  if (!mirror.publication_id.empty())
  {
    payload["publication_id"] = mirror.publication_id;
  }
  if (!mirror.repository_version_id.empty())
  {
    payload["repository_version_id"] = mirror.repository_version_id;
  }
  if (!mirror.synced_at.empty())
  {
    payload["synced_at"] = mirror.synced_at;
  }
  if (mirror.tree_size > 0)
  {
    payload["tree_size"] = mirror.tree_size;
  }
  return payload;
}

nlohmann::json SerializeRegistryPackageRecord(const RegistryPackageRecord &record)
{
  return {
      {"package_id", record.package_id},
      {"namespace", record.package_namespace},
      {"name", record.name},
      {"created_at", record.created_at},
      {"created_by", record.created_by},
      {"visibility", record.visibility},
  };
}

nlohmann::json SerializeRegistryPackageReleaseRecord(const RegistryPackageReleaseRecord &record)
{
  nlohmann::json payload = {
      {"package_id", record.package_id},
      {"version", record.version},
      {"edition", record.edition},
      {"manifest_sha256", record.manifest_sha256},
      {"source_artifact_sha256", record.source_artifact_sha256},
      {"dependencies", record.dependencies},
      {"publisher_id", record.publisher_id},
      {"published_at", record.published_at},
      {"yanked", record.yanked},
      {"yanked_reason", record.yanked_reason},
  };
  return payload;
}

nlohmann::json SerializeRegistryPackageOwnerRecord(const RegistryPackageOwnerRecord &record)
{
  return {
      {"package_id", record.package_id},
      {"owner_id", record.owner_id},
      {"owner_kind", record.owner_kind},
      {"role", record.role},
      {"added_by", record.added_by},
      {"added_at", record.added_at},
  };
}

nlohmann::json SerializeRegistryPublishTokenRecord(const RegistryPublishTokenRecord &record, const bool include_hash)
{
  nlohmann::json payload = {
      {"token_id", record.token_id},
      {"owner_id", record.owner_id},
      {"scopes", record.scopes},
      {"package_patterns", record.package_patterns},
      {"expires_at", record.expires_at},
      {"revoked_at", record.revoked_at},
      {"created_at", record.created_at},
  };
  if (include_hash)
  {
    payload["token_hash"] = record.token_hash;
  }
  return payload;
}

nlohmann::json SerializeRegistryRepositoryRecord(const RegistryRepositoryRecord &record)
{
  return {
      {"repository_id", record.repository_id},
      {"name", record.name},
      {"tenant_id", record.tenant_id},
      {"policy", record.policy},
      {"created_at", record.created_at},
  };
}

nlohmann::json SerializeRegistryRepositoryVersionRecord(const RegistryRepositoryVersionRecord &record)
{
  return {
      {"repository_version_id", record.repository_version_id},
      {"repository_id", record.repository_id},
      {"sequence", record.sequence},
      {"change_kind", record.change_kind},
      {"change_ref", record.change_ref},
      {"created_at", record.created_at},
  };
}

nlohmann::json SerializeRegistryPublicationRecord(const RegistryPublicationRecord &record)
{
  return {
      {"publication_id", record.publication_id},
      {"repository_version_id", record.repository_version_id},
      {"layout_version", record.layout_version},
      {"root_path", record.root_path},
      {"manifest_sha256", record.manifest_sha256},
      {"tree_size", record.tree_size},
      {"created_at", record.created_at},
      {"verified", record.verified},
  };
}

nlohmann::json SerializeRegistryDistributionRecord(const RegistryDistributionRecord &record)
{
  return {
      {"distribution_id", record.distribution_id},
      {"repository_id", record.repository_id},
      {"name", record.name},
      {"base_url", record.base_url},
      {"current_publication_id", record.current_publication_id},
      {"previous_publication_id", record.previous_publication_id},
      {"updated_at", record.updated_at},
  };
}

nlohmann::json SerializeRegistryAuditEventRecord(const RegistryAuditEventRecord &record)
{
  return {
      {"event_id", record.event_id},
      {"actor_id", record.actor_id},
      {"operation", record.operation},
      {"target", record.target},
      {"request_id", record.request_id},
      {"result", record.result},
      {"created_at", record.created_at},
  };
}

CompileContainerRecord CompileContainerRecordFactory::CreateFromRegistration(const nlohmann::json &body) const
{
  return {
      .container_id = body["container_id"].get<std::string>(),
      .worker_id = body["worker_id"].get<std::string>(),
      .tenant_id = body["tenant_id"].get<std::string>(),
      .user_id = body["user_id"].get<std::string>(),
      .current_workspace_id = body["workspace_id"].get<std::string>(),
      .region = body["region"].get<std::string>(),
      .worker_pool_key = body["worker_pool_key"].get<std::string>(),
      .status = body.value("status", "active"),
      .capacity = body.value("capacity", 1),
  };
}

}  // namespace spio::platform
