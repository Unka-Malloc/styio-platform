#include "PlatformCloud/DeveloperWorkspace/WorkerRuntimeFactory.hpp"

#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace pafio::platform
{

namespace
{

std::string EnvString(const OperatingSystemAdapter &os, const char *name, std::string fallback)
{
  const std::optional<std::string> value = os.GetEnv(name);
  if (!value.has_value() || value->empty())
  {
    return fallback;
  }
  return *value;
}

int EnvInt(const OperatingSystemAdapter &os, const char *name, int fallback)
{
  const std::optional<std::string> value = os.GetEnv(name);
  if (!value.has_value() || value->empty())
  {
    return fallback;
  }
  try
  {
    return std::stoi(*value);
  }
  catch (...)
  {
    return fallback;
  }
}

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

WorkerRuntimeConfig LoadWorkerRuntimeConfig(const OperatingSystemAdapter &os)
{
  WorkerRuntimeConfig config;
  config.control_url = EnvString(os, "STYIO_PLATFORM_CONTROL_URL", config.control_url);
  config.worker_id = EnvString(os, "STYIO_PLATFORM_WORKER_ID", config.worker_id);
  config.worker_pool_key = EnvString(os, "STYIO_PLATFORM_WORKER_POOL_KEY", config.worker_pool_key);
  config.workspace_root = EnvString(os, "STYIO_PLATFORM_WORKSPACE_ROOT", config.workspace_root.string());
  config.artifact_root = EnvString(os, "STYIO_PLATFORM_ARTIFACT_ROOT", config.artifact_root.string());
  config.pafio_bin = EnvString(os, "STYIO_PLATFORM_WORKER_PAFIO_BIN", config.pafio_bin);
  config.styio_bin = EnvString(os, "STYIO_PLATFORM_WORKER_STYIO_BIN", config.styio_bin);
  config.compile_container_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_ID", "");
  config.compile_container_tenant_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_TENANT_ID", "");
  config.compile_container_user_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_USER_ID", "");
  config.compile_container_workspace_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_WORKSPACE_ID", "");
  config.compile_container_capacity =
      EnvInt(os, "STYIO_PLATFORM_COMPILE_CONTAINER_CAPACITY", config.compile_container_capacity);
  config.mtls_ca_path = EnvString(os, "STYIO_PLATFORM_MTLS_CA", "");
  config.mtls_cert_path = EnvString(os, "STYIO_PLATFORM_MTLS_CERT", "");
  config.mtls_key_path = EnvString(os, "STYIO_PLATFORM_MTLS_KEY", "");
  config.poll_interval_ms =
      EnvInt(os, "STYIO_PLATFORM_WORKER_POLL_INTERVAL_MS", config.poll_interval_ms);
  return config;
}

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

pafio::ProcessRequest BuildWorkerPafioProcessRequest(
    const WorkerRuntimeConfig &worker,
    const PlatformConfig &platform,
    const WorkerWorkspace &workspace,
    const nlohmann::json &job_request)
{
  if (worker.pafio_bin.empty())
  {
    throw std::runtime_error("STYIO_PLATFORM_WORKER_PAFIO_BIN must identify the Pafio executable");
  }
  if (worker.styio_bin.empty())
  {
    throw std::runtime_error("STYIO_PLATFORM_WORKER_STYIO_BIN must identify the system Styio executable");
  }

  std::vector<std::string> args = {
      "build",
      "--manifest-path",
      workspace.manifest_path.string(),
  };
  if (job_request.contains("workflow") && job_request["workflow"].is_object() &&
      job_request["workflow"].value("dry_run", false))
  {
    args.push_back("--dry-run");
  }
  if (job_request.contains("workflow") && job_request["workflow"].is_object())
  {
    const nlohmann::json &workflow = job_request["workflow"];
    if (workflow.value("frozen", false))
    {
      args.push_back("--frozen");
    }
    else
    {
      if (workflow.value("locked", false))
      {
        args.push_back("--locked");
      }
      if (workflow.value("offline", false))
      {
        args.push_back("--offline");
      }
    }
  }
  if (job_request.contains("profile") && job_request["profile"].is_string())
  {
    args.push_back("--profile");
    args.push_back(job_request["profile"].get<std::string>());
  }

  return {
      .program = worker.pafio_bin,
      .args = std::move(args),
      .working_directory = workspace.checkout_root,
      .environment_overrides = {
          {"PAFIO_STYIO_BIN", worker.styio_bin},
          {"STYIO_PLATFORM_REGION", platform.region},
          {"STYIO_PLATFORM_COMPILE_CONTAINER_ID", worker.compile_container_id},
      },
      .timeout = pafio::kExternalProcessBuildTimeout,
      .error_context = "pafio build for platform worker",
  };
}

}  // namespace pafio::platform
