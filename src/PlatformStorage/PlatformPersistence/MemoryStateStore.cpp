#include "PlatformStorage/PlatformPersistence/MemoryStateStore.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

namespace pafio::platform
{

namespace
{

std::string PaddedNumber(const size_t value, const int width)
{
  std::ostringstream stream;
  stream << std::setw(width) << std::setfill('0') << value;
  return stream.str();
}

}  // namespace

std::string MemoryStateStore::NextJobId()
{
  return "job-" + PaddedNumber(next_job_sequence_++, 12);
}

void MemoryStateStore::SubmitJob(const PlatformJobRecord &job)
{
  jobs_[job.job_id] = job;
  events_[job.job_id].push_back({
      .event_id = "event-queued",
      .job_id = job.job_id,
      .status = "queued",
      .message = "job queued",
  });
}

std::optional<PlatformJobRecord> MemoryStateStore::GetJob(const std::string &job_id) const
{
  const auto found = jobs_.find(job_id);
  if (found == jobs_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

std::vector<JobEventRecord> MemoryStateStore::GetJobEvents(const std::string &job_id) const
{
  const auto found = events_.find(job_id);
  if (found == events_.end())
  {
    return {};
  }
  return found->second;
}

std::optional<PlatformJobRecord> MemoryStateStore::CancelJob(const std::string &job_id, const std::string &reason)
{
  const auto found = jobs_.find(job_id);
  if (found == jobs_.end())
  {
    return std::nullopt;
  }
  PlatformJobRecord &job = found->second;
  job.status = "cancelled";
  job.finished_at = "2026-04-24T00:01:00Z";
  events_[job.job_id].push_back({
      .event_id = "event-cancelled",
      .job_id = job.job_id,
      .status = "cancelled",
      .message = reason,
      .created_at = job.finished_at,
  });
  return job;
}

nlohmann::json MemoryStateStore::RegisterWorker(const nlohmann::json &worker)
{
  workers_[worker["worker_id"].get<std::string>()] = worker;
  return worker;
}

void MemoryStateStore::RequireWorker(const std::string &worker_id) const
{
  if (!workers_.contains(worker_id))
  {
    throw MemoryStateStoreError("worker is not registered");
  }
}

CompileContainerRecord MemoryStateStore::RegisterCompileContainer(const CompileContainerRecord &container)
{
  const auto worker = workers_.find(container.worker_id);
  if (worker == workers_.end())
  {
    throw MemoryStateStoreError("worker is not registered");
  }
  if (worker->second.value("region", "") != container.region ||
      worker->second.value("worker_pool_key", "") != container.worker_pool_key)
  {
    throw MemoryStateStoreError("worker region or pool does not match container");
  }
  const auto existing = compile_containers_.find(container.container_id);
  if (existing != compile_containers_.end() &&
      (existing->second.tenant_id != container.tenant_id || existing->second.user_id != container.user_id))
  {
    throw MemoryStateStoreError("compile container user binding mismatch");
  }
  if (existing != compile_containers_.end() && existing->second.worker_id != container.worker_id)
  {
    throw MemoryStateStoreError("compile container worker owner mismatch");
  }
  compile_containers_[container.container_id] = container;
  return container;
}

std::optional<CompileContainerRecord> MemoryStateStore::GetCompileContainer(const std::string &container_id) const
{
  const auto found = compile_containers_.find(container_id);
  if (found == compile_containers_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

std::optional<CompileContainerRecord> MemoryStateStore::SwitchCompileContainerWorkspace(
    const std::string &container_id,
    const std::string &worker_id,
    const std::string &workspace_id,
    const std::string &reason)
{
  const auto found = compile_containers_.find(container_id);
  if (found == compile_containers_.end())
  {
    return std::nullopt;
  }
  CompileContainerRecord &container = found->second;
  if (container.worker_id != worker_id)
  {
    throw MemoryStateStoreError("worker does not own compile container");
  }
  if (container.status != "active")
  {
    return std::nullopt;
  }
  if (container.current_workspace_id != workspace_id)
  {
    container.current_workspace_id = workspace_id;
    ++container.workspace_generation;
    container.last_switched_at = "2026-04-24T00:01:00Z";
    container.last_switch_reason = reason;
  }
  return container;
}

std::optional<PlatformJobRecord> MemoryStateStore::ClaimJob(
    const std::string &worker_id,
    const std::string &region,
    const std::string &worker_pool_key)
{
  RequireWorker(worker_id);
  for (auto &[job_id, job] : jobs_)
  {
    if (job.status == "queued" && job.region == region && job.worker_pool_key == worker_pool_key)
    {
      job.status = "running";
      job.worker_id = worker_id;
      events_[job_id].push_back({
          .event_id = "event-running",
          .job_id = job_id,
          .status = "running",
          .message = "job claimed by worker",
      });
      return job;
    }
  }
  return std::nullopt;
}

std::optional<PlatformJobRecord> MemoryStateStore::ClaimJobForCompileContainer(
    const std::string &worker_id,
    const std::string &region,
    const std::string &worker_pool_key,
    const std::string &container_id)
{
  RequireWorker(worker_id);
  const auto container_found = compile_containers_.find(container_id);
  if (container_found == compile_containers_.end())
  {
    throw MemoryStateStoreError("compile container is not registered");
  }
  CompileContainerRecord &container = container_found->second;
  if (container.worker_id != worker_id || container.region != region || container.worker_pool_key != worker_pool_key ||
      container.status != "active")
  {
    throw MemoryStateStoreError("compile container is not available for this worker");
  }
  for (auto &[job_id, job] : jobs_)
  {
    if (job.status == "queued" && job.region == region && job.worker_pool_key == worker_pool_key &&
        job.tenant_id == container.tenant_id && job.user_id == container.user_id)
    {
      job.status = "running";
      job.worker_id = worker_id;
      if (container.current_workspace_id != job.workspace_id)
      {
        container.current_workspace_id = job.workspace_id;
        ++container.workspace_generation;
        container.last_switched_at = "2026-04-24T00:01:00Z";
        container.last_switch_reason = "claimJob";
        events_[job_id].push_back({
            .event_id = "event-workspace-switched",
            .job_id = job_id,
            .status = "running",
            .message = "compile container switched workspace",
        });
      }
      events_[job_id].push_back({
          .event_id = "event-running",
          .job_id = job_id,
          .status = "running",
          .message = "job claimed by compile container",
      });
      return job;
    }
  }
  return std::nullopt;
}

std::optional<PlatformJobRecord> MemoryStateStore::HeartbeatJob(
    const std::string &job_id,
    const std::string &worker_id,
    const std::string &message)
{
  const auto found = jobs_.find(job_id);
  if (found == jobs_.end() || found->second.worker_id != worker_id)
  {
    return std::nullopt;
  }
  PlatformJobRecord &job = found->second;
  events_[job.job_id].push_back({
      .event_id = "event-heartbeat",
      .job_id = job.job_id,
      .status = job.status,
      .message = message,
  });
  return job;
}

std::optional<PlatformJobRecord> MemoryStateStore::CompleteJob(
    const std::string &job_id,
    const std::string &worker_id,
    const std::string &status,
    const std::string &message,
    const std::vector<ArtifactRecord> &artifacts,
    const nlohmann::json &result)
{
  (void) result;
  const auto found = jobs_.find(job_id);
  if (found == jobs_.end() || found->second.worker_id != worker_id)
  {
    return std::nullopt;
  }
  PlatformJobRecord &job = found->second;
  job.status = status;
  job.finished_at = "2026-04-24T00:02:00Z";
  for (const ArtifactRecord &artifact : artifacts)
  {
    job.artifacts.push_back(artifact);
  }
  events_[job.job_id].push_back({
      .event_id = "event-completed",
      .job_id = job.job_id,
      .status = job.status,
      .message = message,
      .created_at = job.finished_at,
  });
  return job;
}

nlohmann::json MemoryStateStore::RegisterWorkgroupCluster(const std::string &workgroup_id, const nlohmann::json &cluster)
{
  workgroups_[workgroup_id][cluster["cluster_id"].get<std::string>()] = cluster;
  return cluster;
}

nlohmann::json MemoryStateStore::ListWorkgroupClusters(const std::string &workgroup_id) const
{
  nlohmann::json clusters = nlohmann::json::array();
  if (const auto workgroup = workgroups_.find(workgroup_id); workgroup != workgroups_.end())
  {
    for (const auto &[cluster_id, cluster] : workgroup->second)
    {
      (void) cluster_id;
      clusters.push_back(cluster);
    }
  }
  return clusters;
}

void MemoryStateStore::RecordMirrorState(
    const std::string &mirror_id,
    const std::string &region,
    const std::string &origin,
    const std::string &freshness,
    const std::string &replay_cursor,
    const std::string &publication_id,
    const std::string &repository_version_id,
    const std::string &synced_at,
    const int tree_size)
{
  mirrors_[mirror_id] = MirrorCursorRecord{
      .mirror_id = mirror_id,
      .region = region,
      .origin = origin,
      .freshness = freshness,
      .replay_cursor = replay_cursor,
      .publication_id = publication_id,
      .repository_version_id = repository_version_id,
      .synced_at = synced_at,
      .tree_size = tree_size,
  };
}

std::optional<MirrorCursorRecord> MemoryStateStore::GetMirrorState(const std::string &mirror_id) const
{
  const auto found = mirrors_.find(mirror_id);
  if (found == mirrors_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

bool MemoryStateStore::HasPublishedRelease(const std::string &release_key) const
{
  return published_releases_.contains(release_key);
}

void MemoryStateStore::RecordPublishedRelease(const std::string &release_key, const nlohmann::json &release)
{
  published_releases_[release_key] = release;
}

nlohmann::json MemoryStateStore::ListPublishedReleasePayloads() const
{
  nlohmann::json releases = nlohmann::json::array();
  for (const auto &[release_key, release] : published_releases_)
  {
    (void) release_key;
    releases.push_back(release);
  }
  return releases;
}

void MemoryStateStore::UpsertRegistryPackage(const RegistryPackageRecord &record)
{
  registry_packages_[record.package_id] = record;
}

std::optional<RegistryPackageRecord> MemoryStateStore::GetRegistryPackage(const std::string &package_id) const
{
  const auto found = registry_packages_.find(package_id);
  if (found == registry_packages_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

nlohmann::json MemoryStateStore::ListRegistryPackages() const
{
  nlohmann::json packages = nlohmann::json::array();
  for (const auto &[package_id, record] : registry_packages_)
  {
    (void) package_id;
    packages.push_back(SerializeRegistryPackageRecord(record));
  }
  return packages;
}

void MemoryStateStore::UpsertRegistryPackageRelease(const RegistryPackageReleaseRecord &record)
{
  registry_releases_[record.package_id + "@" + record.version] = record;
}

std::optional<RegistryPackageReleaseRecord> MemoryStateStore::GetRegistryPackageRelease(
    const std::string &package_id,
    const std::string &version) const
{
  const auto found = registry_releases_.find(package_id + "@" + version);
  if (found == registry_releases_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

nlohmann::json MemoryStateStore::ListRegistryPackageReleases(const std::string &package_id) const
{
  nlohmann::json releases = nlohmann::json::array();
  for (const auto &[release_key, record] : registry_releases_)
  {
    (void) release_key;
    if (record.package_id == package_id)
    {
      releases.push_back(SerializeRegistryPackageReleaseRecord(record));
    }
  }
  return releases;
}

bool MemoryStateStore::SetRegistryPackageReleaseYanked(
    const std::string &package_id,
    const std::string &version,
    const bool yanked,
    const std::string &reason)
{
  const auto found = registry_releases_.find(package_id + "@" + version);
  if (found == registry_releases_.end())
  {
    return false;
  }
  found->second.yanked = yanked;
  found->second.yanked_reason = reason;
  return true;
}

void MemoryStateStore::AddRegistryPackageOwner(const RegistryPackageOwnerRecord &record)
{
  registry_owners_[record.package_id + "/" + record.owner_id] = record;
}

bool MemoryStateStore::RemoveRegistryPackageOwner(const std::string &package_id, const std::string &owner_id)
{
  return registry_owners_.erase(package_id + "/" + owner_id) > 0;
}

bool MemoryStateStore::HasRegistryPackageOwner(const std::string &package_id, const std::string &owner_id) const
{
  return registry_owners_.contains(package_id + "/" + owner_id);
}

nlohmann::json MemoryStateStore::ListRegistryPackageOwners(const std::string &package_id) const
{
  nlohmann::json owners = nlohmann::json::array();
  for (const auto &[owner_key, record] : registry_owners_)
  {
    (void) owner_key;
    if (record.package_id == package_id)
    {
      owners.push_back(SerializeRegistryPackageOwnerRecord(record));
    }
  }
  return owners;
}

std::string MemoryStateStore::NextRegistryTokenId()
{
  return "tok-" + PaddedNumber(next_registry_token_sequence_++, 12);
}

void MemoryStateStore::UpsertRegistryPublishToken(const RegistryPublishTokenRecord &record)
{
  registry_tokens_[record.token_id] = record;
}

std::optional<RegistryPublishTokenRecord> MemoryStateStore::GetRegistryPublishToken(const std::string &token_id) const
{
  const auto found = registry_tokens_.find(token_id);
  if (found == registry_tokens_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

std::optional<RegistryPublishTokenRecord> MemoryStateStore::FindRegistryPublishTokenByHash(const std::string &token_hash) const
{
  for (const auto &[token_id, record] : registry_tokens_)
  {
    (void) token_id;
    if (record.token_hash == token_hash)
    {
      return record;
    }
  }
  return std::nullopt;
}

nlohmann::json MemoryStateStore::ListRegistryPublishTokens(const std::string &owner_id) const
{
  nlohmann::json tokens = nlohmann::json::array();
  for (const auto &[token_id, record] : registry_tokens_)
  {
    (void) token_id;
    if (owner_id.empty() || record.owner_id == owner_id)
    {
      tokens.push_back(SerializeRegistryPublishTokenRecord(record));
    }
  }
  return tokens;
}

bool MemoryStateStore::RevokeRegistryPublishToken(const std::string &token_id, const std::string &revoked_at)
{
  const auto found = registry_tokens_.find(token_id);
  if (found == registry_tokens_.end())
  {
    return false;
  }
  found->second.revoked_at = revoked_at;
  return true;
}

int MemoryStateStore::NextRepositoryVersionSequence(const std::string &repository_id) const
{
  int max_sequence = 0;
  for (const auto &[version_id, record] : registry_repository_versions_)
  {
    (void) version_id;
    if (record.repository_id == repository_id && record.sequence > max_sequence)
    {
      max_sequence = record.sequence;
    }
  }
  return max_sequence + 1;
}

void MemoryStateStore::UpsertRegistryRepository(const RegistryRepositoryRecord &record)
{
  registry_repositories_[record.repository_id] = record;
}

nlohmann::json MemoryStateStore::ListRegistryRepositories() const
{
  nlohmann::json repositories = nlohmann::json::array();
  for (const auto &[repository_id, record] : registry_repositories_)
  {
    (void) repository_id;
    repositories.push_back(SerializeRegistryRepositoryRecord(record));
  }
  return repositories;
}

void MemoryStateStore::RecordRegistryRepositoryVersion(const RegistryRepositoryVersionRecord &record)
{
  registry_repository_versions_[record.repository_version_id] = record;
}

nlohmann::json MemoryStateStore::ListRegistryRepositoryVersions(const std::string &repository_id) const
{
  nlohmann::json versions = nlohmann::json::array();
  for (const auto &[version_id, record] : registry_repository_versions_)
  {
    (void) version_id;
    if (record.repository_id == repository_id)
    {
      versions.push_back(SerializeRegistryRepositoryVersionRecord(record));
    }
  }
  return versions;
}

void MemoryStateStore::RecordRegistryPublication(const RegistryPublicationRecord &record)
{
  registry_publications_[record.publication_id] = record;
}

std::optional<RegistryPublicationRecord> MemoryStateStore::GetRegistryPublication(const std::string &publication_id) const
{
  const auto found = registry_publications_.find(publication_id);
  if (found == registry_publications_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

nlohmann::json MemoryStateStore::ListRegistryPublications() const
{
  nlohmann::json publications = nlohmann::json::array();
  for (const auto &[publication_id, record] : registry_publications_)
  {
    (void) publication_id;
    publications.push_back(SerializeRegistryPublicationRecord(record));
  }
  return publications;
}

void MemoryStateStore::UpsertRegistryDistribution(const RegistryDistributionRecord &record)
{
  registry_distributions_[record.distribution_id] = record;
}

std::optional<RegistryDistributionRecord> MemoryStateStore::GetRegistryDistribution(const std::string &distribution_id) const
{
  const auto found = registry_distributions_.find(distribution_id);
  if (found == registry_distributions_.end())
  {
    return std::nullopt;
  }
  return found->second;
}

nlohmann::json MemoryStateStore::ListRegistryDistributions() const
{
  nlohmann::json distributions = nlohmann::json::array();
  for (const auto &[distribution_id, record] : registry_distributions_)
  {
    (void) distribution_id;
    distributions.push_back(SerializeRegistryDistributionRecord(record));
  }
  return distributions;
}

std::string MemoryStateStore::NextRegistryAuditEventId()
{
  return "audit-" + PaddedNumber(next_registry_audit_sequence_++, 12);
}

void MemoryStateStore::RecordRegistryAuditEvent(const RegistryAuditEventRecord &record)
{
  registry_audit_events_.push_back(record);
}

nlohmann::json MemoryStateStore::ListRegistryAuditEvents() const
{
  nlohmann::json events = nlohmann::json::array();
  for (const RegistryAuditEventRecord &event : registry_audit_events_)
  {
    events.push_back(SerializeRegistryAuditEventRecord(event));
  }
  return events;
}

}  // namespace pafio::platform
