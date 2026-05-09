#pragma once

#include "PlatformCore/Config.hpp"
#include "PlatformStorage/PlatformPersistence/StateRecords.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace spio::platform
{

struct SqlMigration
{
  std::string id;
  std::string sql;
};

class PostgresStoreError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

bool LooksLikePostgresDsn(std::string_view value);
bool PostgresDriverAvailable();
std::vector<SqlMigration> CloudKernelMigrations();
std::string ClaimJobSql();
std::string CompleteJobSql();

class PostgresStore
{
public:
  explicit PostgresStore(std::string dsn);

  static bool IsAvailable() { return PostgresDriverAvailable(); }

  void ApplyMigrations() const;
  void UpsertNode(const PlatformConfig &config) const;
  std::string NextJobId() const;
  void SubmitJob(const PlatformJobRecord &job) const;
  std::optional<PlatformJobRecord> GetJob(const std::string &job_id) const;
  std::vector<JobEventRecord> GetJobEvents(const std::string &job_id) const;
  std::optional<PlatformJobRecord> CancelJob(const std::string &job_id, const std::string &reason) const;
  nlohmann::json RegisterWorker(const nlohmann::json &worker) const;
  CompileContainerRecord RegisterCompileContainer(const CompileContainerRecord &container) const;
  std::optional<CompileContainerRecord> GetCompileContainer(const std::string &container_id) const;
  std::optional<CompileContainerRecord> SwitchCompileContainerWorkspace(
      const std::string &container_id,
      const std::string &worker_id,
      const std::string &workspace_id,
      const std::string &reason) const;
  std::optional<PlatformJobRecord> ClaimJob(
      const std::string &worker_id,
      const std::string &region,
      const std::string &worker_pool_key) const;
  std::optional<PlatformJobRecord> ClaimJobForCompileContainer(
      const std::string &worker_id,
      const std::string &region,
      const std::string &worker_pool_key,
      const std::string &container_id) const;
  std::optional<PlatformJobRecord> HeartbeatJob(
      const std::string &job_id,
      const std::string &worker_id,
      const std::string &message) const;
  std::optional<PlatformJobRecord> CompleteJob(
      const std::string &job_id,
      const std::string &worker_id,
      const std::string &status,
      const std::string &message,
      const std::vector<ArtifactRecord> &artifacts,
      const nlohmann::json &result) const;
  nlohmann::json RegisterWorkgroupCluster(const std::string &workgroup_id, const nlohmann::json &cluster) const;
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
      int tree_size = 0) const;
  std::optional<MirrorCursorRecord> GetMirrorState(const std::string &mirror_id) const;

  void UpsertRegistryPackage(const RegistryPackageRecord &record) const;
  std::optional<RegistryPackageRecord> GetRegistryPackage(const std::string &package_id) const;
  nlohmann::json ListRegistryPackages() const;
  void UpsertRegistryPackageRelease(const RegistryPackageReleaseRecord &record) const;
  std::optional<RegistryPackageReleaseRecord> GetRegistryPackageRelease(
      const std::string &package_id,
      const std::string &version) const;
  nlohmann::json ListRegistryPackageReleases(const std::string &package_id) const;
  bool SetRegistryPackageReleaseYanked(
      const std::string &package_id,
      const std::string &version,
      bool yanked,
      const std::string &reason) const;

  void AddRegistryPackageOwner(const RegistryPackageOwnerRecord &record) const;
  bool RemoveRegistryPackageOwner(const std::string &package_id, const std::string &owner_id) const;
  bool HasRegistryPackageOwner(const std::string &package_id, const std::string &owner_id) const;
  nlohmann::json ListRegistryPackageOwners(const std::string &package_id) const;

  std::string NextRegistryTokenId() const;
  void UpsertRegistryPublishToken(const RegistryPublishTokenRecord &record) const;
  std::optional<RegistryPublishTokenRecord> GetRegistryPublishToken(const std::string &token_id) const;
  std::optional<RegistryPublishTokenRecord> FindRegistryPublishTokenByHash(const std::string &token_hash) const;
  nlohmann::json ListRegistryPublishTokens(const std::string &owner_id = {}) const;
  bool RevokeRegistryPublishToken(const std::string &token_id, const std::string &revoked_at) const;

  int NextRepositoryVersionSequence(const std::string &repository_id) const;
  void UpsertRegistryRepository(const RegistryRepositoryRecord &record) const;
  nlohmann::json ListRegistryRepositories() const;
  void RecordRegistryRepositoryVersion(const RegistryRepositoryVersionRecord &record) const;
  nlohmann::json ListRegistryRepositoryVersions(const std::string &repository_id) const;
  void RecordRegistryPublication(const RegistryPublicationRecord &record) const;
  std::optional<RegistryPublicationRecord> GetRegistryPublication(const std::string &publication_id) const;
  nlohmann::json ListRegistryPublications() const;
  void UpsertRegistryDistribution(const RegistryDistributionRecord &record) const;
  std::optional<RegistryDistributionRecord> GetRegistryDistribution(const std::string &distribution_id) const;
  nlohmann::json ListRegistryDistributions() const;

  std::string NextRegistryAuditEventId() const;
  void RecordRegistryAuditEvent(const RegistryAuditEventRecord &record) const;
  nlohmann::json ListRegistryAuditEvents() const;

private:
  std::string dsn_;
};

}  // namespace spio::platform
