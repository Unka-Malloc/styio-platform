#pragma once

#include "PlatformCore/Config.hpp"
#include "PlatformCore/System/OperatingSystemAdapter.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace spio::platform
{

struct WorkerRuntimeConfig
{
  std::string control_url = "http://127.0.0.1:8787/api/styio-platform/v1";
  std::string worker_id = "worker-local";
  std::string worker_pool_key = "default";
  std::filesystem::path workspace_root = ".styio-platform/workspaces";
  std::filesystem::path artifact_root = ".styio-platform/artifacts";
  std::string pafio_bin = "pafio";
  std::string styio_bin = "styio";
  std::string compile_container_id;
  std::string compile_container_tenant_id;
  std::string compile_container_user_id;
  std::string compile_container_workspace_id;
  int compile_container_capacity = 1;
  std::string mtls_ca_path;
  std::string mtls_cert_path;
  std::string mtls_key_path;
  int poll_interval_ms = 2000;
};

WorkerRuntimeConfig LoadWorkerRuntimeConfig(const OperatingSystemAdapter &os);

struct WorkerCompileContainerSpec
{
  bool enabled = false;
  std::string container_id;
  std::string worker_id;
  std::string tenant_id;
  std::string user_id;
  std::string workspace_id;
  std::string region;
  std::string worker_pool_key;
  int capacity = 1;

  nlohmann::json RegistrationRequest() const;
};

class WorkerCompileContainerFactory
{
public:
  WorkerCompileContainerSpec Create(const WorkerRuntimeConfig &worker, const PlatformConfig &platform) const;
};

struct WorkerWorkspace
{
  std::filesystem::path manifest_path;
  std::filesystem::path checkout_root;
  std::filesystem::path artifact_root;
  std::filesystem::path stdout_path;
  std::filesystem::path stderr_path;
  std::filesystem::path result_path;
};

spio::ProcessRequest BuildWorkerPafioProcessRequest(
    const WorkerRuntimeConfig &worker,
    const PlatformConfig &platform,
    const WorkerWorkspace &workspace,
    const nlohmann::json &job_request);

class WorkerWorkspaceFactory
{
public:
  explicit WorkerWorkspaceFactory(const OperatingSystemAdapter &os);

  WorkerWorkspace Create(const WorkerRuntimeConfig &worker, const nlohmann::json &job) const;

private:
  const OperatingSystemAdapter &os_;
};

}  // namespace spio::platform
