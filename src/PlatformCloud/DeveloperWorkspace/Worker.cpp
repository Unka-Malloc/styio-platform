#include "PlatformCloud/DeveloperWorkspace/Worker.hpp"

#include "PlatformCloud/DeveloperWorkspace/WorkerRuntimeFactory.hpp"
#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"
#include "PlatformCore/System/OperatingSystemAdapter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

struct ParsedUrl
{
  std::string scheme = "http";
  std::string host;
  std::string port = "80";
  std::string base_path;
  std::string origin;
};

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

WorkerRuntimeConfig LoadWorkerRuntimeConfig(const OperatingSystemAdapter &os)
{
  WorkerRuntimeConfig config;
  config.control_url = EnvString(os, "STYIO_PLATFORM_CONTROL_URL", config.control_url);
  config.worker_id = EnvString(os, "STYIO_PLATFORM_WORKER_ID", config.worker_id);
  config.worker_pool_key = EnvString(os, "STYIO_PLATFORM_WORKER_POOL_KEY", config.worker_pool_key);
  config.workspace_root = EnvString(os, "STYIO_PLATFORM_WORKSPACE_ROOT", config.workspace_root.string());
  config.artifact_root = EnvString(os, "STYIO_PLATFORM_ARTIFACT_ROOT", config.artifact_root.string());
  config.spio_bin = EnvString(os, "STYIO_PLATFORM_WORKER_SPIO_BIN", config.spio_bin);
  config.styio_bin = EnvString(os, "STYIO_PLATFORM_WORKER_STYIO_BIN", config.styio_bin);
  config.compile_container_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_ID", "");
  config.compile_container_tenant_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_TENANT_ID", "");
  config.compile_container_user_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_USER_ID", "");
  config.compile_container_workspace_id = EnvString(os, "STYIO_PLATFORM_COMPILE_CONTAINER_WORKSPACE_ID", "");
  config.compile_container_capacity = EnvInt(os, "STYIO_PLATFORM_COMPILE_CONTAINER_CAPACITY", config.compile_container_capacity);
  config.mtls_ca_path = EnvString(os, "STYIO_PLATFORM_MTLS_CA", "");
  config.mtls_cert_path = EnvString(os, "STYIO_PLATFORM_MTLS_CERT", "");
  config.mtls_key_path = EnvString(os, "STYIO_PLATFORM_MTLS_KEY", "");
  config.poll_interval_ms = EnvInt(os, "STYIO_PLATFORM_WORKER_POLL_INTERVAL_MS", config.poll_interval_ms);
  return config;
}

ParsedUrl ParseHttpUrl(const std::string &url)
{
  std::string scheme;
  size_t prefix_size = 0;
  std::string default_port;
  if (url.starts_with("http://"))
  {
    scheme = "http";
    prefix_size = std::string_view("http://").size();
    default_port = "80";
  }
  else if (url.starts_with("https://"))
  {
    scheme = "https";
    prefix_size = std::string_view("https://").size();
    default_port = "443";
  }
  else
  {
    throw std::runtime_error("only http:// or https:// control URLs are supported");
  }
  std::string rest = url.substr(prefix_size);
  const size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string base_path = slash == std::string::npos ? "" : rest.substr(slash);
  const size_t colon = authority.rfind(':');
  ParsedUrl parsed;
  parsed.scheme = scheme;
  parsed.port = default_port;
  if (colon == std::string::npos)
  {
    parsed.host = authority;
  }
  else
  {
    parsed.host = authority.substr(0, colon);
    parsed.port = authority.substr(colon + 1);
  }
  parsed.base_path = base_path.empty() ? "" : base_path;
  if (parsed.host.empty())
  {
    throw std::runtime_error("control URL host is empty");
  }
  parsed.origin = parsed.scheme + "://" + authority;
  return parsed;
}

std::string JoinUrlPath(const std::string &base, const std::string &suffix)
{
  if (base.empty())
  {
    return suffix;
  }
  if (base.back() == '/')
  {
    return base.substr(0, base.size() - 1) + suffix;
  }
  return base + suffix;
}

nlohmann::json HttpJson(
    const OperatingSystemAdapter &os,
    const ParsedUrl &url,
    const std::string &method,
    const std::string &path,
    const nlohmann::json &body,
    const WorkerRuntimeConfig &worker)
{
  const std::string payload = body.is_null() ? "" : body.dump();
  if (url.scheme == "https")
  {
    std::vector<std::string> args = {
        "-sS",
        "-f",
        "-X",
        method,
        url.origin + JoinUrlPath(url.base_path, path),
        "-H",
        "Content-Type: application/json",
        "-H",
        "X-Styio-Mtls-Uri-San: spiffe://styio-platform/tenant/platform/role/worker/node/" + worker.worker_id,
        "--data-binary",
        "@-",
    };
    if (!worker.mtls_ca_path.empty())
    {
      args.push_back("--cacert");
      args.push_back(worker.mtls_ca_path);
    }
    if (!worker.mtls_cert_path.empty())
    {
      args.push_back("--cert");
      args.push_back(worker.mtls_cert_path);
    }
    if (!worker.mtls_key_path.empty())
    {
      args.push_back("--key");
      args.push_back(worker.mtls_key_path);
    }
    spio::ProcessResult result = os.RunProcess({
        .program = "curl",
        .args = std::move(args),
        .timeout = spio::kExternalProcessProbeTimeout,
        .max_stdout_bytes = 16U << 20,
        .max_stderr_bytes = 1U << 20,
        .stdin_text = payload,
        .error_context = "worker HTTPS control-plane request",
    });
    if (result.exit_code != 0 || result.timed_out)
    {
      throw std::runtime_error("control plane returned non-success status: " + spio::DescribeProcessFailure(result));
    }
    return nlohmann::json::parse(result.stdout_text);
  }

  std::ostringstream request;
  request << method << " " << JoinUrlPath(url.base_path, path) << " HTTP/1.1\r\n";
  request << "Host: " << url.host << "\r\n";
  request << "Connection: close\r\n";
  request << "Content-Type: application/json\r\n";
  request << "X-Styio-Mtls-Uri-San: spiffe://styio-platform/tenant/platform/role/worker/node/" << worker.worker_id << "\r\n";
  request << "Content-Length: " << payload.size() << "\r\n\r\n";
  request << payload;

  const std::string raw = os.SendTcpRequest({
      .host = url.host,
      .port = url.port,
      .payload = request.str(),
      .context = "worker HTTP control-plane request",
  });

  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos)
  {
    throw std::runtime_error("malformed HTTP response from control plane");
  }
  const std::string status_line = raw.substr(0, raw.find("\r\n"));
  if (status_line.find(" 2") == std::string::npos)
  {
    throw std::runtime_error("control plane returned non-success status: " + status_line + " body=" + raw.substr(header_end + 4));
  }
  return nlohmann::json::parse(raw.substr(header_end + 4));
}

void WriteText(const OperatingSystemAdapter &os, const fs::path &path, const std::string &text)
{
  os.WriteTextFile(path, text);
}

std::vector<std::string> BuildSpioArgs(const nlohmann::json &job_request, const std::string &styio_bin)
{
  std::vector<std::string> args = {"build", "--manifest-path", job_request.at("manifest_path").get<std::string>()};
  if (job_request.contains("workflow") && job_request["workflow"].is_object() &&
      job_request["workflow"].value("dry_run", false))
  {
    args.push_back("--dry-run");
  }
  if (!styio_bin.empty())
  {
    args.push_back("--styio-bin");
    args.push_back(styio_bin);
  }
  if (job_request.contains("profile") && job_request["profile"].is_string())
  {
    args.push_back("--profile");
    args.push_back(job_request["profile"].get<std::string>());
  }
  return args;
}

nlohmann::json Artifact(
    const nlohmann::json &job,
    const fs::path &artifact_root,
    const std::string &name,
    const std::string &kind)
{
  const std::string object_key = BuildArtifactObjectKey(
      job.at("tenant_id").get<std::string>(),
      job.at("workspace_id").get<std::string>(),
      job.at("job_id").get<std::string>(),
      name);
  return {
      {"artifact_id", name},
      {"object_key", (artifact_root / object_key).string()},
      {"kind", kind},
  };
}

void Complete(
    const OperatingSystemAdapter &os,
    const ParsedUrl &control,
    const WorkerRuntimeConfig &worker,
    const std::string &job_id,
    const std::string &status,
    const std::string &message,
    const nlohmann::json &artifacts,
    const nlohmann::json &result)
{
  (void) HttpJson(
      os,
      control,
      "POST",
      "/jobs/" + job_id + "/complete",
      {
          {"worker_id", worker.worker_id},
          {"status", status},
          {"message", message},
          {"artifacts", artifacts},
          {"result", result},
      },
      worker);
}

void Heartbeat(
    const OperatingSystemAdapter &os,
    const ParsedUrl &control,
    const WorkerRuntimeConfig &worker,
    const std::string &job_id,
    const std::string &message)
{
  (void) HttpJson(
      os,
      control,
      "POST",
      "/jobs/" + job_id + "/heartbeat",
      {
          {"worker_id", worker.worker_id},
          {"message", message},
      },
      worker);
}

void RunHeartbeatLoop(
    const OperatingSystemAdapter &os,
    const ParsedUrl &control,
    const WorkerRuntimeConfig &worker,
    const std::string &job_id,
    std::atomic_bool &done)
{
  while (!done.load())
  {
    os.SleepFor(std::chrono::milliseconds(std::max(worker.poll_interval_ms, 1000)));
    if (done.load())
    {
      break;
    }
    try
    {
      Heartbeat(os, control, worker, job_id, "worker build still running");
    }
    catch (const std::exception &error)
    {
      std::cerr << "worker heartbeat failed: " << error.what() << "\n";
    }
  }
}

void ExecuteClaimedJob(
    const OperatingSystemAdapter &os,
    const ParsedUrl &control,
    const PlatformConfig &platform,
    const WorkerRuntimeConfig &worker,
    const WorkerWorkspaceFactory &workspace_factory,
    const nlohmann::json &job)
{
  const std::string job_id = job.at("job_id").get<std::string>();
  const nlohmann::json job_request = job.at("job_request");
  WorkerWorkspace workspace;
  try
  {
    workspace = workspace_factory.Create(worker, job);
  }
  catch (const std::exception &error)
  {
    Complete(os, control, worker, job_id, "failed", error.what(), nlohmann::json::array(), {});
    return;
  }

  const nlohmann::json &source = job_request.at("source");
  const std::string origin = source.at("origin").get<std::string>();
  spio::ProcessResult clone = os.RunProcess({
      .program = "git",
      .args = {"clone", origin, workspace.checkout_root.string()},
      .timeout = spio::kExternalProcessStepTimeout,
      .error_context = "git clone for platform worker",
  });
  if (clone.exit_code != 0 || clone.timed_out)
  {
    WriteText(os, workspace.stderr_path, clone.stderr_text);
    Complete(os, control, worker, job_id, "failed", "git clone failed", nlohmann::json::array(), {{"clone", clone.exit_code}});
    return;
  }

  if (source.contains("requested_revision") && source["requested_revision"].is_string())
  {
    const std::string revision = source["requested_revision"].get<std::string>();
    spio::ProcessResult checkout = os.RunProcess({
        .program = "git",
        .args = {"checkout", revision},
        .working_directory = workspace.checkout_root,
        .timeout = spio::kExternalProcessStepTimeout,
        .error_context = "git checkout for platform worker",
    });
    if (checkout.exit_code != 0 || checkout.timed_out)
    {
      WriteText(os, workspace.stderr_path, checkout.stderr_text);
      Complete(os, control, worker, job_id, "failed", "git checkout failed", nlohmann::json::array(), {{"checkout", checkout.exit_code}});
      return;
    }
  }

  Heartbeat(os, control, worker, job_id, "worker starting build");
  std::atomic_bool heartbeat_done = false;
  std::thread heartbeat_thread(
      RunHeartbeatLoop,
      std::cref(os),
      std::cref(control),
      std::cref(worker),
      std::cref(job_id),
      std::ref(heartbeat_done));
  spio::ProcessResult build;
  try
  {
    build = os.RunProcess({
        .program = worker.spio_bin,
        .args = BuildSpioArgs(job_request, worker.styio_bin),
        .working_directory = workspace.checkout_root,
        .environment_overrides = {
            {"SPIO_STYIO_BIN", worker.styio_bin},
            {"STYIO_PLATFORM_REGION", platform.region},
            {"STYIO_PLATFORM_COMPILE_CONTAINER_ID", worker.compile_container_id},
        },
        .timeout = spio::kExternalProcessBuildTimeout,
        .error_context = "spio build for platform worker",
    });
  }
  catch (const std::exception &error)
  {
    heartbeat_done = true;
    heartbeat_thread.join();
    WriteText(os, workspace.stderr_path, error.what());
    Complete(os, control, worker, job_id, "failed", "spio build failed to launch", nlohmann::json::array(), {{"error", error.what()}});
    return;
  }
  heartbeat_done = true;
  heartbeat_thread.join();

  WriteText(os, workspace.stdout_path, build.stdout_text);
  WriteText(os, workspace.stderr_path, build.stderr_text);
  const bool succeeded = build.exit_code == 0 && !build.timed_out && !build.terminated_by_signal;
  const nlohmann::json result = {
      {"exit_code", build.exit_code},
      {"timed_out", build.timed_out},
      {"terminated_by_signal", build.terminated_by_signal},
      {"stdout_truncated", build.stdout_truncated},
      {"stderr_truncated", build.stderr_truncated},
  };
  WriteText(os, workspace.result_path, result.dump(2) + "\n");

  nlohmann::json artifacts = nlohmann::json::array({
      Artifact(job, worker.artifact_root, "stdout.log", "log"),
      Artifact(job, worker.artifact_root, "stderr.log", "log"),
      Artifact(job, worker.artifact_root, "result.json", "metadata"),
  });
  Complete(os, control, worker, job_id, succeeded ? "succeeded" : "failed", succeeded ? "build completed" : "build failed", artifacts, result);
}

}  // namespace

int RunWorker(const PlatformConfig &config, WorkerOptions options)
{
  try
  {
    const OperatingSystemAdapter &os = DefaultOperatingSystemAdapter();
    const WorkerRuntimeConfig worker = LoadWorkerRuntimeConfig(os);
    const ParsedUrl control = ParseHttpUrl(worker.control_url);
    const WorkerCompileContainerFactory compile_container_factory;
    const WorkerCompileContainerSpec compile_container = compile_container_factory.Create(worker, config);
    const WorkerWorkspaceFactory workspace_factory(os);
    os.CreateDirectories(worker.workspace_root);
    os.CreateDirectories(worker.artifact_root);

    (void) HttpJson(
        os,
        control,
        "POST",
        "/workers/register",
        {
            {"worker_id", worker.worker_id},
            {"region", config.region},
            {"worker_pool_key", worker.worker_pool_key},
            {"capacity", 1},
        },
        worker);

    if (compile_container.enabled)
    {
      (void) HttpJson(
          os,
          control,
          "POST",
          "/compile-containers/register",
          compile_container.RegistrationRequest(),
          worker);
    }

    do
    {
      nlohmann::json claim_request = {
          {"worker_id", worker.worker_id},
          {"region", config.region},
          {"worker_pool_key", worker.worker_pool_key},
      };
      if (compile_container.enabled)
      {
        claim_request["compile_container_id"] = compile_container.container_id;
      }
      const nlohmann::json claim = HttpJson(
          os,
          control,
          "POST",
          "/jobs/claim",
          claim_request,
          worker);
      const nlohmann::json payload = claim.at("payload");
      if (payload.value("claimed", false))
      {
        ExecuteClaimedJob(os, control, config, worker, workspace_factory, payload.at("job"));
      }
      else if (!options.once)
      {
        os.SleepFor(std::chrono::milliseconds(worker.poll_interval_ms));
      }
    } while (!options.once);
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "styio-platform worker failed: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace spio::platform
