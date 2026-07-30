#pragma once

#include "PlatformStorage/PlatformPersistence/StateRecords.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pafio::platform
{

class MemoryStateStoreError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

class MemoryStateStore
{
public:
  std::string NextJobId();
  void SubmitJob(const PlatformJobRecord &job);
  std::optional<PlatformJobRecord> GetJob(const std::string &job_id) const;
  std::vector<JobEventRecord> GetJobEvents(const std::string &job_id) const;
  std::optional<PlatformJobRecord> CancelJob(const std::string &job_id, const std::string &reason);

  nlohmann::json RegisterWorker(const nlohmann::json &worker);
  CompileContainerRecord RegisterCompileContainer(const CompileContainerRecord &container);
  std::optional<CompileContainerRecord> GetCompileContainer(const std::string &container_id) const;
  std::optional<CompileContainerRecord> SwitchCompileContainerWorkspace(
      const std::string &container_id,
      const std::string &worker_id,
      const std::string &workspace_id,
      const std::string &reason);

  std::optional<PlatformJobRecord> ClaimJob(
      const std::string &worker_id,
      const std::string &region,
      const std::string &worker_pool_key);
  std::optional<PlatformJobRecord> ClaimJobForCompileContainer(
      const std::string &worker_id,
      const std::string &region,
      const std::string &worker_pool_key,
      const std::string &container_id);
  std::optional<PlatformJobRecord> HeartbeatJob(
      const std::string &job_id,
      const std::string &worker_id,
      const std::string &message);
  std::optional<PlatformJobRecord> CompleteJob(
      const std::string &job_id,
      const std::string &worker_id,
      const std::string &status,
      const std::string &message,
      const std::vector<ArtifactRecord> &artifacts,
      const nlohmann::json &result);

  nlohmann::json RegisterWorkgroupCluster(const std::string &workgroup_id, const nlohmann::json &cluster);
  nlohmann::json ListWorkgroupClusters(const std::string &workgroup_id) const;

  void RecordMirrorState(
      const std::string &mirror_id,
      const std::string &region,
      const std::string &origin,
      const std::string &freshness,
      const std::string &replay_cursor,
      const std::string &publication_id = {},
      const std::string &repository_version_id = {},
      const std::string &synced_at = {},
      int tree_size = 0);
  std::optional<MirrorCursorRecord> GetMirrorState(const std::string &mirror_id) const;

  bool HasPublishedRelease(const std::string &release_key) const;
  void RecordPublishedRelease(const std::string &release_key, const nlohmann::json &release);
  nlohmann::json ListPublishedReleasePayloads() const;

  void UpsertRegistryPackage(const RegistryPackageRecord &record);
  std::optional<RegistryPackageRecord> GetRegistryPackage(const std::string &package_id) const;
  nlohmann::json ListRegistryPackages() const;
  void UpsertRegistryPackageRelease(const RegistryPackageReleaseRecord &record);
  std::optional<RegistryPackageReleaseRecord> GetRegistryPackageRelease(
      const std::string &package_id,
      const std::string &version) const;
  nlohmann::json ListRegistryPackageReleases(const std::string &package_id) const;
  bool SetRegistryPackageReleaseYanked(
      const std::string &package_id,
      const std::string &version,
      bool yanked,
      const std::string &reason);

  void AddRegistryPackageOwner(const RegistryPackageOwnerRecord &record);
  bool RemoveRegistryPackageOwner(const std::string &package_id, const std::string &owner_id);
  bool HasRegistryPackageOwner(const std::string &package_id, const std::string &owner_id) const;
  nlohmann::json ListRegistryPackageOwners(const std::string &package_id) const;

  std::string NextRegistryTokenId();
  void UpsertRegistryPublishToken(const RegistryPublishTokenRecord &record);
  std::optional<RegistryPublishTokenRecord> GetRegistryPublishToken(const std::string &token_id) const;
  std::optional<RegistryPublishTokenRecord> FindRegistryPublishTokenByHash(const std::string &token_hash) const;
  nlohmann::json ListRegistryPublishTokens(const std::string &owner_id = {}) const;
  bool RevokeRegistryPublishToken(const std::string &token_id, const std::string &revoked_at);

  int NextRepositoryVersionSequence(const std::string &repository_id) const;
  void UpsertRegistryRepository(const RegistryRepositoryRecord &record);
  nlohmann::json ListRegistryRepositories() const;
  void RecordRegistryRepositoryVersion(const RegistryRepositoryVersionRecord &record);
  nlohmann::json ListRegistryRepositoryVersions(const std::string &repository_id) const;
  void RecordRegistryPublication(const RegistryPublicationRecord &record);
  std::optional<RegistryPublicationRecord> GetRegistryPublication(const std::string &publication_id) const;
  nlohmann::json ListRegistryPublications() const;
  void UpsertRegistryDistribution(const RegistryDistributionRecord &record);
  std::optional<RegistryDistributionRecord> GetRegistryDistribution(const std::string &distribution_id) const;
  nlohmann::json ListRegistryDistributions() const;

  std::string NextRegistryAuditEventId();
  void RecordRegistryAuditEvent(const RegistryAuditEventRecord &record);
  nlohmann::json ListRegistryAuditEvents() const;

private:
  void RequireWorker(const std::string &worker_id) const;

  std::map<std::string, PlatformJobRecord> jobs_;
  std::map<std::string, std::vector<JobEventRecord>> events_;
  std::map<std::string, nlohmann::json> workers_;
  std::map<std::string, CompileContainerRecord> compile_containers_;
  std::map<std::string, std::map<std::string, nlohmann::json>> workgroups_;
  std::map<std::string, MirrorCursorRecord> mirrors_;
  std::map<std::string, nlohmann::json> published_releases_;
  std::map<std::string, RegistryPackageRecord> registry_packages_;
  std::map<std::string, RegistryPackageReleaseRecord> registry_releases_;
  std::map<std::string, RegistryPackageOwnerRecord> registry_owners_;
  std::map<std::string, RegistryPublishTokenRecord> registry_tokens_;
  std::map<std::string, RegistryRepositoryRecord> registry_repositories_;
  std::map<std::string, RegistryRepositoryVersionRecord> registry_repository_versions_;
  std::map<std::string, RegistryPublicationRecord> registry_publications_;
  std::map<std::string, RegistryDistributionRecord> registry_distributions_;
  std::vector<RegistryAuditEventRecord> registry_audit_events_;
  size_t next_job_sequence_ = 1;
  size_t next_registry_token_sequence_ = 1;
  size_t next_registry_audit_sequence_ = 1;
};

}  // namespace pafio::platform
