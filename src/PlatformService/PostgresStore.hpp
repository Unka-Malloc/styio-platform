#pragma once

#include "PlatformService/Config.hpp"
#include "PlatformService/JobQueue.hpp"

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

struct MirrorCursorRecord
{
  std::string mirror_id;
  std::string region;
  std::string origin;
  std::string freshness;
  std::string replay_cursor;
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
  void SubmitJob(const PlatformJobRecord &job) const;
  std::optional<PlatformJobRecord> GetJob(const std::string &job_id) const;
  std::vector<JobEventRecord> GetJobEvents(const std::string &job_id) const;
  std::optional<PlatformJobRecord> CancelJob(const std::string &job_id, const std::string &reason) const;
  nlohmann::json RegisterWorker(const nlohmann::json &worker) const;
  std::optional<PlatformJobRecord> ClaimJob(
      const std::string &worker_id,
      const std::string &region,
      const std::string &worker_pool_key) const;
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
      const std::string &replay_cursor) const;
  std::optional<MirrorCursorRecord> GetMirrorState(const std::string &mirror_id) const;

private:
  std::string dsn_;
};

}  // namespace spio::platform
