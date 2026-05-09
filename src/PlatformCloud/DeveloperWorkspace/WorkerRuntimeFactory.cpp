#include "PlatformCloud/DeveloperWorkspace/WorkerRuntimeFactory.hpp"

#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"

#include <stdexcept>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

bool IsSafeRelativePath(const fs::path &path)
{
  if (path.empty() || path.is_absolute())
  {
    return false;
  }
  for (const fs::path &part : path)
  {
    if (part == "..")
    {
      return false;
    }
  }
  return true;
}

}  // namespace

nlohmann::json WorkerCompileContainerSpec::RegistrationRequest() const
{
  return {
      {"container_id", container_id},
      {"worker_id", worker_id},
      {"tenant_id", tenant_id},
      {"user_id", user_id},
      {"workspace_id", workspace_id},
      {"region", region},
      {"worker_pool_key", worker_pool_key},
      {"capacity", capacity},
  };
}

WorkerCompileContainerSpec WorkerCompileContainerFactory::Create(
    const WorkerRuntimeConfig &worker,
    const PlatformConfig &platform) const
{
  const bool any = !worker.compile_container_id.empty() || !worker.compile_container_tenant_id.empty() ||
                   !worker.compile_container_user_id.empty() || !worker.compile_container_workspace_id.empty();
  const bool all = !worker.compile_container_id.empty() && !worker.compile_container_tenant_id.empty() &&
                   !worker.compile_container_user_id.empty() && !worker.compile_container_workspace_id.empty();
  if (any && !all)
  {
    throw std::runtime_error(
        "STYIO_PLATFORM_COMPILE_CONTAINER_ID, _TENANT_ID, _USER_ID, and _WORKSPACE_ID must be set together");
  }
  if (!all)
  {
    return {};
  }
  return {
      .enabled = true,
      .container_id = worker.compile_container_id,
      .worker_id = worker.worker_id,
      .tenant_id = worker.compile_container_tenant_id,
      .user_id = worker.compile_container_user_id,
      .workspace_id = worker.compile_container_workspace_id,
      .region = platform.region,
      .worker_pool_key = worker.worker_pool_key,
      .capacity = worker.compile_container_capacity,
  };
}

WorkerWorkspaceFactory::WorkerWorkspaceFactory(const OperatingSystemAdapter &os) : os_(os) {}

WorkerWorkspace WorkerWorkspaceFactory::Create(const WorkerRuntimeConfig &worker, const nlohmann::json &job) const
{
  const std::string job_id = job.at("job_id").get<std::string>();
  const nlohmann::json job_request = job.at("job_request");
  const fs::path manifest_path(job_request.at("manifest_path").get<std::string>());
  if (!IsSafeRelativePath(manifest_path))
  {
    throw std::runtime_error("manifest_path must be relative for source fetch jobs");
  }

  const fs::path checkout_root = worker.compile_container_id.empty()
                                     ? worker.workspace_root / job_id / "source"
                                     : worker.workspace_root / "containers" / worker.compile_container_id / "source";
  const fs::path artifact_root = (worker.artifact_root / BuildArtifactObjectKey(
                                      job.at("tenant_id").get<std::string>(),
                                      job.at("workspace_id").get<std::string>(),
                                      job_id,
                                      "result.json"))
                                     .parent_path();
  os_.RemoveAll(checkout_root);
  os_.CreateDirectories(checkout_root.parent_path());
  os_.CreateDirectories(artifact_root);

  return {
      .manifest_path = manifest_path,
      .checkout_root = checkout_root,
      .artifact_root = artifact_root,
      .stdout_path = artifact_root / "stdout.log",
      .stderr_path = artifact_root / "stderr.log",
      .result_path = artifact_root / "result.json",
  };
}

}  // namespace spio::platform
