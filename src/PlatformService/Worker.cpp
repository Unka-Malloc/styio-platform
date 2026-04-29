#include "PlatformService/Worker.hpp"

#include "PlatformService/Http.hpp"
#include "PlatformService/ObjectStore.hpp"
#include "SpioCore/Process.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

struct ParsedUrl
{
  std::string host;
  std::string port = "80";
  std::string base_path;
};

struct WorkerRuntimeConfig
{
  std::string control_url = "http://127.0.0.1:8787/api/styio-platform/v1";
  std::string worker_id = "worker-local";
  std::string worker_pool_key = "linux/x86_64/build/nightly/minimal";
  fs::path workspace_root = ".styio-platform/workspaces";
  fs::path artifact_root = ".styio-platform/artifacts";
  std::string spio_bin = "spio";
  std::string styio_bin = "styio";
  int poll_interval_ms = 2000;
};

std::string EnvString(const char *name, std::string fallback)
{
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0')
  {
    return fallback;
  }
  return value;
}

int EnvInt(const char *name, int fallback)
{
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0')
  {
    return fallback;
  }
  try
  {
    return std::stoi(value);
  }
  catch (...)
  {
    return fallback;
  }
}

WorkerRuntimeConfig LoadWorkerRuntimeConfig()
{
  WorkerRuntimeConfig config;
  config.control_url = EnvString("STYIO_PLATFORM_CONTROL_URL", config.control_url);
  config.worker_id = EnvString("STYIO_PLATFORM_WORKER_ID", config.worker_id);
  config.worker_pool_key = EnvString("STYIO_PLATFORM_WORKER_POOL_KEY", config.worker_pool_key);
  config.workspace_root = EnvString("STYIO_PLATFORM_WORKSPACE_ROOT", config.workspace_root.string());
  config.artifact_root = EnvString("STYIO_PLATFORM_ARTIFACT_ROOT", config.artifact_root.string());
  config.spio_bin = EnvString("STYIO_PLATFORM_WORKER_SPIO_BIN", config.spio_bin);
  config.styio_bin = EnvString("STYIO_PLATFORM_WORKER_STYIO_BIN", config.styio_bin);
  config.poll_interval_ms = EnvInt("STYIO_PLATFORM_WORKER_POLL_INTERVAL_MS", config.poll_interval_ms);
  return config;
}

ParsedUrl ParseHttpUrl(const std::string &url)
{
  constexpr std::string_view prefix = "http://";
  if (!url.starts_with(prefix))
  {
    throw std::runtime_error("only http:// control URLs are supported");
  }
  std::string rest = url.substr(prefix.size());
  const size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string base_path = slash == std::string::npos ? "" : rest.substr(slash);
  const size_t colon = authority.rfind(':');
  ParsedUrl parsed;
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

std::string ReadAllFromSocket(int fd)
{
  std::string out;
  std::array<char, 4096> buffer{};
  while (true)
  {
    const ssize_t rc = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (rc == 0)
    {
      break;
    }
    if (rc < 0)
    {
      throw std::runtime_error("failed reading HTTP response");
    }
    out.append(buffer.data(), static_cast<size_t>(rc));
  }
  return out;
}

void SendAllToSocket(int fd, const std::string &payload)
{
  size_t written = 0;
  while (written < payload.size())
  {
    const ssize_t rc = ::send(fd, payload.data() + written, payload.size() - written, 0);
    if (rc <= 0)
    {
      throw std::runtime_error("failed writing HTTP request");
    }
    written += static_cast<size_t>(rc);
  }
}

int ConnectTcp(const ParsedUrl &url)
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const int rc = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &addresses);
  if (rc != 0)
  {
    throw std::runtime_error(std::string("failed resolving control host: ") + gai_strerror(rc));
  }
  int fd = -1;
  for (addrinfo *candidate = addresses; candidate != nullptr; candidate = candidate->ai_next)
  {
    fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (fd < 0)
    {
      continue;
    }
    if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
    {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  if (fd < 0)
  {
    throw std::runtime_error("failed connecting to control plane");
  }
  return fd;
}

nlohmann::json HttpJson(
    const ParsedUrl &url,
    const std::string &method,
    const std::string &path,
    const nlohmann::json &body,
    const std::string &worker_id)
{
  const std::string payload = body.is_null() ? "" : body.dump();
  std::ostringstream request;
  request << method << " " << JoinUrlPath(url.base_path, path) << " HTTP/1.1\r\n";
  request << "Host: " << url.host << "\r\n";
  request << "Connection: close\r\n";
  request << "Content-Type: application/json\r\n";
  request << "X-Styio-Mtls-Uri-San: spiffe://styio-platform/tenant/platform/role/worker/node/" << worker_id << "\r\n";
  request << "Content-Length: " << payload.size() << "\r\n\r\n";
  request << payload;

  const int fd = ConnectTcp(url);
  SendAllToSocket(fd, request.str());
  const std::string raw = ReadAllFromSocket(fd);
  close(fd);

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

void WriteText(const fs::path &path, const std::string &text)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
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
    const ParsedUrl &control,
    const WorkerRuntimeConfig &worker,
    const std::string &job_id,
    const std::string &status,
    const std::string &message,
    const nlohmann::json &artifacts,
    const nlohmann::json &result)
{
  (void) HttpJson(
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
      worker.worker_id);
}

void Heartbeat(const ParsedUrl &control, const WorkerRuntimeConfig &worker, const std::string &job_id, const std::string &message)
{
  (void) HttpJson(
      control,
      "POST",
      "/jobs/" + job_id + "/heartbeat",
      {
          {"worker_id", worker.worker_id},
          {"message", message},
      },
      worker.worker_id);
}

void RunHeartbeatLoop(
    const ParsedUrl &control,
    const WorkerRuntimeConfig &worker,
    const std::string &job_id,
    std::atomic_bool &done)
{
  while (!done.load())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(worker.poll_interval_ms, 1000)));
    if (done.load())
    {
      break;
    }
    try
    {
      Heartbeat(control, worker, job_id, "worker build still running");
    }
    catch (const std::exception &error)
    {
      std::cerr << "worker heartbeat failed: " << error.what() << "\n";
    }
  }
}

void ExecuteClaimedJob(const ParsedUrl &control, const PlatformConfig &platform, const WorkerRuntimeConfig &worker, const nlohmann::json &job)
{
  const std::string job_id = job.at("job_id").get<std::string>();
  const nlohmann::json job_request = job.at("job_request");
  const fs::path manifest_path(job_request.at("manifest_path").get<std::string>());
  const fs::path checkout_root = worker.workspace_root / job_id / "source";
  const fs::path artifact_root = (worker.artifact_root / BuildArtifactObjectKey(
                                      job.at("tenant_id").get<std::string>(),
                                      job.at("workspace_id").get<std::string>(),
                                      job_id,
                                      "result.json"))
                                     .parent_path();
  const fs::path stdout_path = artifact_root / "stdout.log";
  const fs::path stderr_path = artifact_root / "stderr.log";
  const fs::path result_path = artifact_root / "result.json";

  if (!IsSafeRelativePath(manifest_path))
  {
    Complete(control, worker, job_id, "failed", "manifest_path must be relative for git clone jobs", nlohmann::json::array(), {});
    return;
  }

  fs::remove_all(checkout_root);
  fs::create_directories(checkout_root.parent_path());
  fs::create_directories(artifact_root);

  const nlohmann::json &source = job_request.at("source");
  const std::string origin = source.at("origin").get<std::string>();
  spio::ProcessResult clone = spio::RunProcessChecked({
      .program = "git",
      .args = {"clone", origin, checkout_root.string()},
      .timeout = spio::kExternalProcessStepTimeout,
      .error_context = "git clone for platform worker",
  });
  if (clone.exit_code != 0 || clone.timed_out)
  {
    WriteText(stderr_path, clone.stderr_text);
    Complete(control, worker, job_id, "failed", "git clone failed", nlohmann::json::array(), {{"clone", clone.exit_code}});
    return;
  }

  if (source.contains("requested_revision") && source["requested_revision"].is_string())
  {
    const std::string revision = source["requested_revision"].get<std::string>();
    spio::ProcessResult checkout = spio::RunProcessChecked({
        .program = "git",
        .args = {"checkout", revision},
        .working_directory = checkout_root,
        .timeout = spio::kExternalProcessStepTimeout,
        .error_context = "git checkout for platform worker",
    });
    if (checkout.exit_code != 0 || checkout.timed_out)
    {
      WriteText(stderr_path, checkout.stderr_text);
      Complete(control, worker, job_id, "failed", "git checkout failed", nlohmann::json::array(), {{"checkout", checkout.exit_code}});
      return;
    }
  }

  Heartbeat(control, worker, job_id, "worker starting build");
  std::atomic_bool heartbeat_done = false;
  std::thread heartbeat_thread(RunHeartbeatLoop, std::cref(control), std::cref(worker), std::cref(job_id), std::ref(heartbeat_done));
  spio::ProcessResult build;
  try
  {
    build = spio::RunProcessChecked({
        .program = worker.spio_bin,
        .args = BuildSpioArgs(job_request, worker.styio_bin),
        .working_directory = checkout_root,
        .environment_overrides = {
            {"SPIO_STYIO_BIN", worker.styio_bin},
            {"STYIO_PLATFORM_REGION", platform.region},
        },
        .timeout = spio::kExternalProcessBuildTimeout,
        .error_context = "spio build for platform worker",
    });
  }
  catch (const std::exception &error)
  {
    heartbeat_done = true;
    heartbeat_thread.join();
    WriteText(stderr_path, error.what());
    Complete(control, worker, job_id, "failed", "spio build failed to launch", nlohmann::json::array(), {{"error", error.what()}});
    return;
  }
  heartbeat_done = true;
  heartbeat_thread.join();

  WriteText(stdout_path, build.stdout_text);
  WriteText(stderr_path, build.stderr_text);
  const bool succeeded = build.exit_code == 0 && !build.timed_out && !build.terminated_by_signal;
  const nlohmann::json result = {
      {"exit_code", build.exit_code},
      {"timed_out", build.timed_out},
      {"terminated_by_signal", build.terminated_by_signal},
      {"stdout_truncated", build.stdout_truncated},
      {"stderr_truncated", build.stderr_truncated},
  };
  WriteText(result_path, result.dump(2) + "\n");

  nlohmann::json artifacts = nlohmann::json::array({
      Artifact(job, worker.artifact_root, "stdout.log", "log"),
      Artifact(job, worker.artifact_root, "stderr.log", "log"),
      Artifact(job, worker.artifact_root, "result.json", "metadata"),
  });
  Complete(control, worker, job_id, succeeded ? "succeeded" : "failed", succeeded ? "build completed" : "build failed", artifacts, result);
}

}  // namespace

int RunWorker(const PlatformConfig &config, WorkerOptions options)
{
  try
  {
    const WorkerRuntimeConfig worker = LoadWorkerRuntimeConfig();
    const ParsedUrl control = ParseHttpUrl(worker.control_url);
    fs::create_directories(worker.workspace_root);
    fs::create_directories(worker.artifact_root);

    (void) HttpJson(
        control,
        "POST",
        "/workers/register",
        {
            {"worker_id", worker.worker_id},
            {"region", config.region},
            {"worker_pool_key", worker.worker_pool_key},
            {"capacity", 1},
        },
        worker.worker_id);

    do
    {
      const nlohmann::json claim = HttpJson(
          control,
          "POST",
          "/jobs/claim",
          {
              {"worker_id", worker.worker_id},
              {"region", config.region},
              {"worker_pool_key", worker.worker_pool_key},
          },
          worker.worker_id);
      const nlohmann::json payload = claim.at("payload");
      if (payload.value("claimed", false))
      {
        ExecuteClaimedJob(control, config, worker, payload.at("job"));
      }
      else if (!options.once)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(worker.poll_interval_ms));
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
