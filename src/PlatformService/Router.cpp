#include "PlatformService/Router.hpp"

#include "PlatformService/ObjectStore.hpp"
#include "PlatformService/PostgresStore.hpp"
#include "SpioCore/Errors.hpp"
#include "SpioCore/Sha256.hpp"
#include "SpioManifest/Manifest.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

bool IsInternalRole(const MtlsIdentity &identity)
{
  return identity.role == "control-plane" || identity.role == "worker" || identity.role == "mirror" ||
         identity.role == "registry-writer" || identity.role == "operator" || identity.role == "cluster-registrar";
}

bool IsRegistryOperation(std::string_view operation_id)
{
  return operation_id == "registryStatus" || operation_id == "publishRelease" || operation_id == "verifyRegistry";
}

bool UsesPostgresState(const PlatformConfig &config)
{
  return config.state_backend == "postgres";
}

bool RoleIn(const MtlsIdentity &identity, std::initializer_list<std::string_view> allowed)
{
  for (const std::string_view role : allowed)
  {
    if (identity.role == role)
    {
      return true;
    }
  }
  return false;
}

bool IsAuthorizedForOperation(std::string_view operation_id, const MtlsIdentity &identity)
{
  if (operation_id == "registryStatus")
  {
    return RoleIn(identity, {"control-plane", "registry-writer", "mirror", "operator"});
  }
  if (operation_id == "publishRelease")
  {
    return RoleIn(identity, {"registry-writer", "operator"});
  }
  if (operation_id == "verifyRegistry")
  {
    return RoleIn(identity, {"registry-writer", "mirror", "operator"});
  }
  if (operation_id == "mirrorStatus")
  {
    return RoleIn(identity, {"control-plane", "registry-writer", "mirror", "operator"});
  }
  if (operation_id == "registerWorkgroupCluster")
  {
    return RoleIn(identity, {"control-plane", "operator", "cluster-registrar"});
  }
  if (operation_id == "listWorkgroupClusters")
  {
    return IsInternalRole(identity);
  }
  return true;
}

HttpResponse JsonResponse(int status, nlohmann::json body)
{
  return {.status_code = status, .body = std::move(body)};
}

nlohmann::json RegistryFailureEnvelope(
    std::string message,
    std::string detail,
    std::string category,
    int returncode = 17)
{
  return {
      {"returncode", returncode},
      {"message", std::move(message)},
      {"stdout", ""},
      {"stderr", detail},
      {"error_payload", {
                            {"category", std::move(category)},
                            {"detail", std::move(detail)},
                        }},
  };
}

HttpResponse FailureResponse(
    int status,
    std::string message,
    std::string detail,
    std::string category,
    std::string operation_id,
    int returncode = 17)
{
  if (IsRegistryOperation(operation_id))
  {
    return JsonResponse(
        status,
        RegistryFailureEnvelope(std::move(message), std::move(detail), std::move(category), returncode));
  }
  return JsonResponse(
      status,
      FailureEnvelope(std::move(message), std::move(detail), std::move(category), std::move(operation_id), returncode));
}

bool HasNonEmptyString(const nlohmann::json &body, const std::string &field)
{
  return body.contains(field) && body[field].is_string() && !body[field].get<std::string>().empty();
}

std::optional<std::string> ValidateOptionalStringFields(
    const nlohmann::json &body,
    std::initializer_list<std::string_view> fields)
{
  for (const std::string_view field : fields)
  {
    const std::string key(field);
    if (body.contains(key) && (!body[key].is_string() || body[key].get<std::string>().empty()))
    {
      return key + " must be a non-empty string when present";
    }
  }
  return std::nullopt;
}

std::string PaddedNumber(const size_t value, const int width)
{
  std::ostringstream stream;
  stream << std::setw(width) << std::setfill('0') << value;
  return stream.str();
}

std::string SanitizePathSegment(std::string_view value)
{
  std::string out;
  out.reserve(value.size());
  for (const unsigned char ch : value)
  {
    if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.')
    {
      out.push_back(static_cast<char>(ch));
    }
    else
    {
      out.push_back('_');
    }
  }
  if (out.empty() || out == "." || out == "..")
  {
    return "_";
  }
  return out;
}

std::vector<std::string> SplitPackageName(std::string_view package)
{
  std::vector<std::string> parts;
  std::stringstream stream{std::string(package)};
  std::string item;
  while (std::getline(stream, item, '/'))
  {
    if (!item.empty())
    {
      parts.push_back(SanitizePathSegment(item));
    }
  }
  return parts;
}

bool IsSafeRegistrySegment(std::string_view value)
{
  if (value.empty())
  {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!((first >= 'a' && first <= 'z') || std::isdigit(first) != 0))
  {
    return false;
  }
  for (const unsigned char ch : value)
  {
    if (!((ch >= 'a' && ch <= 'z') || std::isdigit(ch) != 0 || ch == '-' || ch == '_'))
    {
      return false;
    }
  }
  return true;
}

std::string JoinPathParts(const std::vector<std::string> &parts, const size_t begin, const size_t end)
{
  std::ostringstream stream;
  for (size_t index = begin; index < end; ++index)
  {
    if (index > begin)
    {
      stream << "/";
    }
    stream << parts[index];
  }
  return stream.str();
}

std::optional<std::string> ValidatePackageName(std::string_view package)
{
  const size_t slash = package.find('/');
  if (slash == std::string_view::npos || slash != package.rfind('/'))
  {
    return "package must use namespace/name form";
  }
  if (!IsSafeRegistrySegment(package.substr(0, slash)) || !IsSafeRegistrySegment(package.substr(slash + 1)))
  {
    return "package must use lowercase namespace/name segments";
  }
  return std::nullopt;
}

bool IsSafeWorkgroupId(std::string_view value)
{
  return IsSafeRegistrySegment(value);
}

bool LooksLikeHttpEndpoint(std::string_view value)
{
  return value.starts_with("http://") || value.starts_with("https://");
}

std::optional<std::string> ValidateStringArray(const nlohmann::json &body, const std::string &field)
{
  if (!body.contains(field))
  {
    return std::nullopt;
  }
  if (!body[field].is_array())
  {
    return field + " must be an array";
  }
  for (const nlohmann::json &entry : body[field])
  {
    if (!entry.is_string() || entry.get<std::string>().empty())
    {
      return field + " entries must be non-empty strings";
    }
  }
  return std::nullopt;
}

nlohmann::json WorkgroupPolicyPayload(const PlatformConfig &config)
{
  return {
      {"enabled", config.workgroup.enabled},
      {"workgroup_id", config.workgroup.id},
      {"trust_domain", config.workgroup.trust_domain},
      {"registration_policy", config.workgroup.registration_policy},
      {"registration_tenant", config.workgroup.registration_tenant},
      {"registration_token_required", !config.workgroup.registration_token.empty()},
      {"allowed_registration_roles", {"operator", "control-plane", "cluster-registrar"}},
      {"allowed_read_roles", {"operator", "control-plane", "cluster-registrar", "worker", "mirror", "registry-writer"}},
  };
}

std::optional<std::string> ValidateWorkgroupClusterRegistration(
    const std::string &workgroup_id,
    const nlohmann::json &body)
{
  if (!IsSafeWorkgroupId(workgroup_id))
  {
    return "workgroup_id must start with a lowercase letter or digit and contain only lowercase letters, digits, hyphen, or underscore";
  }
  for (const std::string field : {"cluster_id", "region", "node_id", "control_plane_endpoint"})
  {
    if (!HasNonEmptyString(body, field))
    {
      return field + " is required";
    }
  }
  if (!IsSafeWorkgroupId(body["cluster_id"].get<std::string>()))
  {
    return "cluster_id must start with a lowercase letter or digit and contain only lowercase letters, digits, hyphen, or underscore";
  }
  if (!LooksLikeHttpEndpoint(body["control_plane_endpoint"].get<std::string>()))
  {
    return "control_plane_endpoint must be an http or https endpoint";
  }
  for (const std::string field : {"registry_endpoint", "mirror_endpoint", "internal_control_plane_endpoint"})
  {
    if (HasNonEmptyString(body, field) && !LooksLikeHttpEndpoint(body[field].get<std::string>()))
    {
      return field + " must be an http or https endpoint";
    }
  }
  if (const std::optional<std::string> error = ValidateStringArray(body, "roles"); error.has_value())
  {
    return error;
  }
  if (body.contains("labels") && !body["labels"].is_object())
  {
    return "labels must be an object when present";
  }
  return std::nullopt;
}

nlohmann::json BuildWorkgroupClusterRecord(
    const std::string &workgroup_id,
    const nlohmann::json &body,
    const PlatformConfig &config,
    const std::optional<MtlsIdentity> &identity)
{
  nlohmann::json roles = body.value("roles", config.roles);
  nlohmann::json record = {
      {"workgroup_id", workgroup_id},
      {"cluster_id", body["cluster_id"].get<std::string>()},
      {"region", body["region"].get<std::string>()},
      {"node_id", body["node_id"].get<std::string>()},
      {"control_plane_endpoint", body["control_plane_endpoint"].get<std::string>()},
      {"roles", std::move(roles)},
      {"labels", body.value("labels", nlohmann::json::object())},
      {"trust_domain", body.value("trust_domain", config.workgroup.trust_domain)},
      {"registration_policy", config.workgroup.registration_policy},
      {"status", "registered"},
      {"registered_by", identity.has_value() ? SerializeMtlsIdentity(*identity)
                                             : nlohmann::json{{"role", "anonymous"}, {"tenant_id", "anonymous"}, {"node_id", "anonymous"}}},
  };
  for (const std::string field : {"registry_endpoint", "mirror_endpoint", "internal_control_plane_endpoint"})
  {
    if (HasNonEmptyString(body, field))
    {
      record[field] = body[field].get<std::string>();
    }
  }
  return record;
}

std::string RegistryIndexPathForPackage(std::string_view package)
{
  const std::vector<std::string> parts = SplitPackageName(package);
  return "index/" + JoinPathParts(parts, 0, parts.size() - 1) + "/" + parts.back() + ".jsonl";
}

std::string RegistryReleaseKey(const std::string &package, const std::string &version)
{
  return package + "@" + version;
}

bool JsonLineHasRelease(const std::string &line, const std::string &package, const std::string &version)
{
  try
  {
    const nlohmann::json entry = nlohmann::json::parse(line);
    return entry.value("package", "") == package && entry.value("version", "") == version;
  }
  catch (...)
  {
    return false;
  }
}

bool ReleaseExistsOnDisk(const fs::path &registry_root, const std::string &package, const std::string &version)
{
  const fs::path index_path = registry_root / RegistryIndexPathForPackage(package);
  std::ifstream in(index_path);
  if (!in)
  {
    return false;
  }
  std::string line;
  while (std::getline(in, line))
  {
    if (JsonLineHasRelease(line, package, version))
    {
      return true;
    }
  }
  return false;
}

size_t CountRegularFiles(const fs::path &root)
{
  std::error_code ec;
  if (!fs::exists(root, ec))
  {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec))
  {
    if (entry.is_regular_file(ec))
    {
      ++count;
    }
  }
  return count;
}

size_t CountIndexReleases(const fs::path &index_root)
{
  std::error_code ec;
  if (!fs::exists(index_root, ec))
  {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(index_root, ec))
  {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".jsonl")
    {
      continue;
    }
    std::ifstream in(entry.path());
    std::string line;
    while (std::getline(in, line))
    {
      if (!line.empty())
      {
        ++count;
      }
    }
  }
  return count;
}

size_t CountNamespaces(const fs::path &index_root)
{
  std::error_code ec;
  if (!fs::exists(index_root, ec))
  {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::directory_iterator(index_root, ec))
  {
    if (entry.is_directory(ec))
    {
      ++count;
    }
  }
  return count;
}

void WriteJsonFileIfMissing(const fs::path &path, const nlohmann::json &payload)
{
  if (fs::exists(path))
  {
    return;
  }
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << payload.dump(2) << "\n";
}

void AppendJsonLine(const fs::path &path, const nlohmann::json &payload)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::app);
  out << payload.dump() << "\n";
}

struct PublishDraft
{
  std::string package;
  std::string version;
  std::string publisher_id;
  fs::path archive_path;
  int dependencies = 0;
  int dev_dependencies = 0;
};

PublishDraft BuildPublishDraft(
    const nlohmann::json &body,
    const PlatformConfig &config,
    const std::optional<MtlsIdentity> &identity)
{
  PublishDraft draft;
  draft.publisher_id =
      body.value("publisher_id", identity.has_value() ? identity->node_id : std::string("control-plane"));

  if (HasNonEmptyString(body, "manifest_path"))
  {
    const spio::ManifestDocument manifest = spio::LoadManifest(body["manifest_path"].get<std::string>());
    if (!manifest.package.has_value())
    {
      throw spio::ValidationError("manifest_path must point to a package manifest");
    }
    draft.package = manifest.package->name;
    draft.version = manifest.package->version;
    draft.dependencies = static_cast<int>(manifest.package->dependencies.size());
    draft.dev_dependencies = static_cast<int>(manifest.package->dev_dependencies.size());
  }

  if (HasNonEmptyString(body, "package"))
  {
    draft.package = body["package"].get<std::string>();
  }
  if (HasNonEmptyString(body, "version"))
  {
    draft.version = body["version"].get<std::string>();
  }
  if (draft.package.empty())
  {
    throw spio::ValidationError("package or manifest_path is required");
  }
  if (draft.version.empty())
  {
    throw spio::ValidationError("manifest_path is required when version is not provided");
  }
  if (const std::optional<std::string> error = ValidatePackageName(draft.package); error.has_value())
  {
    throw spio::ValidationError(*error);
  }

  if (HasNonEmptyString(body, "archive_path"))
  {
    draft.archive_path = body["archive_path"].get<std::string>();
  }
  else
  {
    const fs::path staging_dir = fs::path(config.registry.root) / "_staging";
    draft.archive_path =
        staging_dir / (SanitizePathSegment(draft.package) + "-" + SanitizePathSegment(draft.version) + ".spio.src.tar");
    fs::create_directories(staging_dir);
    std::ofstream out(draft.archive_path);
    out << nlohmann::json{
               {"package", draft.package},
               {"version", draft.version},
               {"publisher_id", draft.publisher_id},
           }.dump(2)
        << "\n";
  }
  if (!fs::exists(draft.archive_path) || !fs::is_regular_file(draft.archive_path))
  {
    throw spio::ValidationError("archive_path must point to a readable file");
  }
  return draft;
}

void EnsureRegistryRootInitialized(const PlatformConfig &config)
{
  const fs::path root(config.registry.root);
  fs::create_directories(root);
  fs::create_directories(fs::path(config.registry.key_dir));
  WriteJsonFileIfMissing(
      root / "config.json",
      {
          {"schema_version", 1},
          {"registry_name", config.registry.registry_name},
          {"layout", "spio-registry-v2"},
      });
  WriteJsonFileIfMissing(
      root / "trust" / "root.json",
      {
          {"schema_version", 1},
          {"registry_name", config.registry.registry_name},
          {"keys", nlohmann::json::array()},
      });
}

}  // namespace

std::vector<RouteSpec> BuildPlatformControlPlaneRoutes()
{
  return {
      {.operation_id = "health", .method = HttpMethod::Get, .path = "/health"},
      {.operation_id = "nodeSelf", .method = HttpMethod::Get, .path = "/nodes/self"},
      {.operation_id = "submitJob", .method = HttpMethod::Post, .path = "/jobs"},
      {.operation_id = "getJob", .method = HttpMethod::Get, .path = "/jobs/{job_id}"},
      {.operation_id = "getJobEvents", .method = HttpMethod::Get, .path = "/jobs/{job_id}/events"},
      {.operation_id = "cancelJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/cancel"},
      {.operation_id = "registerWorker", .method = HttpMethod::Post, .path = "/workers/register", .internal = true},
      {.operation_id = "claimJob", .method = HttpMethod::Post, .path = "/jobs/claim", .internal = true},
      {.operation_id = "heartbeatJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/heartbeat", .internal = true},
      {.operation_id = "completeJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/complete", .internal = true},
      {.operation_id = "registerWorkgroupCluster", .method = HttpMethod::Post, .path = "/workgroups/{workgroup_id}/clusters/register", .internal = true},
      {.operation_id = "listWorkgroupClusters", .method = HttpMethod::Get, .path = "/workgroups/{workgroup_id}/clusters", .internal = true},
      {.operation_id = "mirrorStatus", .method = HttpMethod::Get, .path = "/mirrors/{mirror_id}/status"},
  };
}

std::vector<RouteSpec> BuildRegistryControlPlaneRoutes()
{
  return {
      {
          .operation_id = "registryStatus",
          .method = HttpMethod::Get,
          .path = "/api/spio-registry-control/v1/status",
          .internal = true,
      },
      {
          .operation_id = "publishRelease",
          .method = HttpMethod::Post,
          .path = "/api/spio-registry-control/v1/publish",
          .internal = true,
      },
      {
          .operation_id = "verifyRegistry",
          .method = HttpMethod::Post,
          .path = "/api/spio-registry-control/v1/verify",
          .internal = true,
      },
  };
}

PlatformRouter::PlatformRouter(PlatformConfig config)
    : config_(std::move(config)), routes_(BuildPlatformControlPlaneRoutes())
{
  const std::vector<RouteSpec> registry_routes = BuildRegistryControlPlaneRoutes();
  routes_.insert(routes_.end(), registry_routes.begin(), registry_routes.end());
  mirrors_[config_.registry.mirror_id] = RegistryMirrorState{
      .mirror_id = config_.registry.mirror_id,
      .origin = config_.registry.mirror_origin,
      .freshness = "lagging",
      .replay_cursor = "checkpoint-0000",
  };
  if (UsesPostgresState(config_))
  {
    postgres_ = std::make_unique<PostgresStore>(config_.postgres_dsn);
  }
}

HttpResponse PlatformRouter::Dispatch(const HttpRequest &request)
{
  const std::optional<RouteMatch> match = MatchRoute(routes_, request.method, request.path);
  if (!match.has_value())
  {
    return JsonResponse(404, FailureEnvelope("route not found", request.path, "NotFound", "unknown", 17));
  }
  if (const HttpResponse identity_response = RequireIdentity(*match, request); identity_response.status_code != 200)
  {
    return identity_response;
  }

  const std::string &operation = match->route.operation_id;
  if (operation == "health")
  {
    return HandleHealth();
  }
  if (operation == "nodeSelf")
  {
    return HandleNodeSelf();
  }
  if (operation == "submitJob")
  {
    return HandleSubmitJob(request);
  }
  if (operation == "getJob")
  {
    return HandleGetJob(*match);
  }
  if (operation == "getJobEvents")
  {
    return HandleGetJobEvents(*match);
  }
  if (operation == "cancelJob")
  {
    return HandleCancelJob(*match, request);
  }
  if (operation == "registerWorker")
  {
    return HandleRegisterWorker(request);
  }
  if (operation == "claimJob")
  {
    return HandleClaimJob(request);
  }
  if (operation == "heartbeatJob")
  {
    return HandleHeartbeatJob(*match, request);
  }
  if (operation == "completeJob")
  {
    return HandleCompleteJob(*match, request);
  }
  if (operation == "registerWorkgroupCluster")
  {
    return HandleRegisterWorkgroupCluster(*match, request);
  }
  if (operation == "listWorkgroupClusters")
  {
    return HandleListWorkgroupClusters(*match);
  }
  if (operation == "mirrorStatus")
  {
    return HandleMirrorStatus(*match);
  }
  if (operation == "registryStatus")
  {
    return HandleRegistryStatus();
  }
  if (operation == "publishRelease")
  {
    return HandlePublishRelease(request);
  }
  if (operation == "verifyRegistry")
  {
    return HandleVerifyRegistry(request);
  }
  return JsonResponse(500, FailureEnvelope("route handler missing", operation, "InternalError", operation));
}

HttpResponse PlatformRouter::RequireIdentity(const RouteMatch &match, const HttpRequest &request) const
{
  if (!config_.mtls.required)
  {
    return JsonResponse(200, {});
  }
  if (!request.identity.has_value())
  {
    return FailureResponse(
        401,
        "mTLS identity is required",
        "missing client certificate identity",
        "AuthError",
        match.route.operation_id,
        2);
  }
  if (match.route.internal && !IsInternalRole(*request.identity))
  {
    return FailureResponse(
        403,
        "mTLS identity is not authorized",
        "internal route requires a service role",
        "AuthError",
        match.route.operation_id,
        2);
  }
  if (!IsAuthorizedForOperation(match.route.operation_id, *request.identity))
  {
    return FailureResponse(
        403,
        "mTLS identity is not authorized",
        "identity role is not allowed for this operation",
        "AuthError",
        match.route.operation_id,
        2);
  }
  return JsonResponse(200, {});
}

HttpResponse PlatformRouter::HandleHealth() const
{
  const bool postgres_ready =
      !UsesPostgresState(config_) || (LooksLikePostgresDsn(config_.postgres_dsn) && PostgresDriverAvailable());
  const bool ready = postgres_ready && IsObjectStoreProviderImplemented(ParseObjectStoreProvider(config_.object_store.provider));
  nlohmann::json payload = {
      {"service", "styio-platformd"},
      {"status", ready ? "ready" : "degraded"},
      {"region", config_.region},
      {"node_id", config_.node_id},
      {"contract_version", "v1"},
      {"roles", config_.roles},
      {"state_backend", config_.state_backend},
      {"postgres_driver_available", PostgresDriverAvailable()},
  };
  return JsonResponse(200, SuccessEnvelope(ready ? "styio-platform is ready" : "styio-platform is degraded", payload));
}

HttpResponse PlatformRouter::HandleNodeSelf() const
{
  nlohmann::json payload = {
      {"node_id", config_.node_id},
      {"region", config_.region},
      {"roles", config_.roles},
      {"state_backend", config_.state_backend},
      {"postgres_configured", LooksLikePostgresDsn(config_.postgres_dsn)},
      {"postgres_driver_available", PostgresDriverAvailable()},
      {"object_store_provider", ToString(ParseObjectStoreProvider(config_.object_store.provider))},
      {"mtls_required", config_.mtls.required},
  };
  return JsonResponse(200, SuccessEnvelope("resolved current platform node", payload));
}

HttpResponse PlatformRouter::HandleSubmitJob(const HttpRequest &request)
{
  if (const std::optional<std::string> error = ValidateSubmitJobRequest(request.body); error.has_value())
  {
    return JsonResponse(400, FailureEnvelope("job submission rejected", *error, "ValidationError", "submitJob", 2));
  }
  PlatformJobRecord job = BuildQueuedJobRecord(request.body, config_);
  if (postgres_ != nullptr)
  {
    try
    {
      postgres_->SubmitJob(job);
      return JsonResponse(200, SuccessEnvelope("queued platform job", SerializeJobRecord(job)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "job submission failed", error.what(), "PostgresError", "submitJob");
    }
  }
  jobs_[job.job_id] = job;
  events_[job.job_id].push_back({
      .event_id = "event-queued",
      .job_id = job.job_id,
      .status = "queued",
      .message = "job queued",
  });
  return JsonResponse(200, SuccessEnvelope("queued platform job", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleGetJob(const RouteMatch &match) const
{
  if (postgres_ != nullptr)
  {
    try
    {
      const std::optional<PlatformJobRecord> job = postgres_->GetJob(match.parameters.at("job_id"));
      if (!job.has_value())
      {
        return JsonResponse(404, FailureEnvelope("job lookup failed", "job not found", "NotFound", "getJob"));
      }
      return JsonResponse(200, SuccessEnvelope("loaded platform job", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "job lookup failed", error.what(), "PostgresError", "getJob");
    }
  }
  const auto job = jobs_.find(match.parameters.at("job_id"));
  if (job == jobs_.end())
  {
    return JsonResponse(404, FailureEnvelope("job lookup failed", "job not found", "NotFound", "getJob"));
  }
  return JsonResponse(200, SuccessEnvelope("loaded platform job", SerializeJobRecord(job->second)));
}

HttpResponse PlatformRouter::HandleGetJobEvents(const RouteMatch &match) const
{
  const std::string job_id = match.parameters.at("job_id");
  if (postgres_ != nullptr)
  {
    try
    {
      if (!postgres_->GetJob(job_id).has_value())
      {
        return JsonResponse(404, FailureEnvelope("job event lookup failed", "job not found", "NotFound", "getJobEvents"));
      }
      nlohmann::json events = nlohmann::json::array();
      for (const JobEventRecord &event : postgres_->GetJobEvents(job_id))
      {
        events.push_back(SerializeJobEvent(event));
      }
      return JsonResponse(200, SuccessEnvelope("loaded platform job events", {{"job_id", job_id}, {"events", events}}));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "job event lookup failed", error.what(), "PostgresError", "getJobEvents");
    }
  }
  const auto found = events_.find(job_id);
  if (found == events_.end())
  {
    return JsonResponse(404, FailureEnvelope("job event lookup failed", "job not found", "NotFound", "getJobEvents"));
  }
  nlohmann::json events = nlohmann::json::array();
  for (const JobEventRecord &event : found->second)
  {
    events.push_back(SerializeJobEvent(event));
  }
  return JsonResponse(200, SuccessEnvelope("loaded platform job events", {{"job_id", job_id}, {"events", events}}));
}

HttpResponse PlatformRouter::HandleCancelJob(const RouteMatch &match, const HttpRequest &request)
{
  if (!request.body.is_object() || !request.body.contains("reason") || !request.body["reason"].is_string())
  {
    return JsonResponse(400, FailureEnvelope("job cancellation failed", "reason is required", "ValidationError", "cancelJob", 2));
  }
  if (postgres_ != nullptr)
  {
    try
    {
      const std::optional<PlatformJobRecord> job =
          postgres_->CancelJob(match.parameters.at("job_id"), request.body["reason"].get<std::string>());
      if (!job.has_value())
      {
        return JsonResponse(404, FailureEnvelope("job cancellation failed", "job not found or already completed", "NotFound", "cancelJob"));
      }
      return JsonResponse(200, SuccessEnvelope("cancelled platform job", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "job cancellation failed", error.what(), "PostgresError", "cancelJob");
    }
  }
  PlatformJobRecord &job = jobs_[match.parameters.at("job_id")];
  if (job.job_id.empty())
  {
    return JsonResponse(404, FailureEnvelope("job cancellation failed", "job not found", "NotFound", "cancelJob"));
  }
  job.status = "cancelled";
  job.finished_at = "2026-04-24T00:01:00Z";
  events_[job.job_id].push_back({
      .event_id = "event-cancelled",
      .job_id = job.job_id,
      .status = "cancelled",
      .message = request.body["reason"].get<std::string>(),
      .created_at = job.finished_at,
  });
  return JsonResponse(200, SuccessEnvelope("cancelled platform job", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleRegisterWorker(const HttpRequest &request)
{
  for (const std::string field : {"worker_id", "region", "worker_pool_key"})
  {
    if (!request.body.contains(field) || !request.body[field].is_string())
    {
      return JsonResponse(400, FailureEnvelope("worker registration rejected", field + " is required", "ValidationError", "registerWorker", 2));
    }
  }
  const int capacity = request.body.value("capacity", 0);
  if (capacity < 1)
  {
    return JsonResponse(400, FailureEnvelope("worker registration rejected", "capacity must be positive", "ValidationError", "registerWorker", 2));
  }
  nlohmann::json worker = {
      {"worker_id", request.body["worker_id"].get<std::string>()},
      {"region", request.body["region"].get<std::string>()},
      {"worker_pool_key", request.body["worker_pool_key"].get<std::string>()},
      {"capacity", capacity},
      {"status", "registered"},
  };
  if (postgres_ != nullptr)
  {
    try
    {
      return JsonResponse(200, SuccessEnvelope("registered platform worker", postgres_->RegisterWorker(worker)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "worker registration failed", error.what(), "PostgresError", "registerWorker");
    }
  }
  workers_[worker["worker_id"].get<std::string>()] = worker;
  return JsonResponse(200, SuccessEnvelope("registered platform worker", worker));
}

HttpResponse PlatformRouter::HandleClaimJob(const HttpRequest &request)
{
  for (const std::string field : {"worker_id", "region", "worker_pool_key"})
  {
    if (!request.body.contains(field) || !request.body[field].is_string())
    {
      return JsonResponse(400, FailureEnvelope("job claim failed", field + " is required", "ValidationError", "claimJob", 2));
    }
  }
  const std::string worker_id = request.body["worker_id"].get<std::string>();
  const std::string region = request.body["region"].get<std::string>();
  const std::string worker_pool_key = request.body["worker_pool_key"].get<std::string>();
  if (postgres_ != nullptr)
  {
    try
    {
      const std::optional<PlatformJobRecord> job = postgres_->ClaimJob(worker_id, region, worker_pool_key);
      if (!job.has_value())
      {
        return JsonResponse(200, SuccessEnvelope("no platform job available", {{"claimed", false}}));
      }
      return JsonResponse(200, SuccessEnvelope("claimed platform job", {{"claimed", true}, {"job", SerializeJobRecord(*job)}}));
    }
    catch (const PostgresStoreError &error)
    {
      const std::string detail = error.what();
      if (detail.find("worker is not registered") != std::string::npos)
      {
        return JsonResponse(403, FailureEnvelope("job claim failed", "worker is not registered", "WorkerError", "claimJob"));
      }
      return FailureResponse(503, "job claim failed", detail, "PostgresError", "claimJob");
    }
  }
  if (!workers_.contains(worker_id))
  {
    return JsonResponse(403, FailureEnvelope("job claim failed", "worker is not registered", "WorkerError", "claimJob"));
  }
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
      return JsonResponse(200, SuccessEnvelope("claimed platform job", {{"claimed", true}, {"job", SerializeJobRecord(job)}}));
    }
  }
  return JsonResponse(200, SuccessEnvelope("no platform job available", {{"claimed", false}}));
}

HttpResponse PlatformRouter::HandleHeartbeatJob(const RouteMatch &match, const HttpRequest &request)
{
  if (postgres_ != nullptr)
  {
    if (!request.body.contains("worker_id") || !request.body["worker_id"].is_string())
    {
      return JsonResponse(400, FailureEnvelope("job heartbeat failed", "worker_id is required", "ValidationError", "heartbeatJob", 2));
    }
    try
    {
      const std::optional<PlatformJobRecord> job = postgres_->HeartbeatJob(
          match.parameters.at("job_id"),
          request.body["worker_id"].get<std::string>(),
          request.body.value("message", "worker heartbeat"));
      if (!job.has_value())
      {
        return JsonResponse(403, FailureEnvelope("job heartbeat failed", "worker does not own job", "WorkerError", "heartbeatJob"));
      }
      return JsonResponse(200, SuccessEnvelope("recorded platform job heartbeat", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "job heartbeat failed", error.what(), "PostgresError", "heartbeatJob");
    }
  }
  PlatformJobRecord &job = jobs_[match.parameters.at("job_id")];
  if (job.job_id.empty())
  {
    return JsonResponse(404, FailureEnvelope("job heartbeat failed", "job not found", "NotFound", "heartbeatJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != job.worker_id)
  {
    return JsonResponse(403, FailureEnvelope("job heartbeat failed", "worker does not own job", "WorkerError", "heartbeatJob"));
  }
  events_[job.job_id].push_back({
      .event_id = "event-heartbeat",
      .job_id = job.job_id,
      .status = job.status,
      .message = request.body.value("message", "worker heartbeat"),
  });
  return JsonResponse(200, SuccessEnvelope("recorded platform job heartbeat", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleCompleteJob(const RouteMatch &match, const HttpRequest &request)
{
  if (postgres_ != nullptr)
  {
    if (!request.body.contains("worker_id") || !request.body["worker_id"].is_string())
    {
      return JsonResponse(400, FailureEnvelope("job completion failed", "worker_id is required", "ValidationError", "completeJob", 2));
    }
    const std::string status = request.body.value("status", "");
    if (status != "succeeded" && status != "failed" && status != "cancelled")
    {
      return JsonResponse(400, FailureEnvelope("job completion failed", "status must be succeeded, failed, or cancelled", "ValidationError", "completeJob", 2));
    }
    std::vector<ArtifactRecord> artifacts;
    if (request.body.contains("artifacts") && request.body["artifacts"].is_array())
    {
      for (const nlohmann::json &artifact : request.body["artifacts"])
      {
        artifacts.push_back({
            .artifact_id = artifact.value("artifact_id", "artifact"),
            .object_key = artifact.value("object_key", ""),
            .kind = artifact.value("kind", "artifact"),
        });
      }
    }
    try
    {
      const std::optional<PlatformJobRecord> job = postgres_->CompleteJob(
          match.parameters.at("job_id"),
          request.body["worker_id"].get<std::string>(),
          status,
          request.body.value("message", "job completed"),
          artifacts,
          request.body.value("result", nlohmann::json::object()));
      if (!job.has_value())
      {
        return JsonResponse(403, FailureEnvelope("job completion failed", "worker does not own job", "WorkerError", "completeJob"));
      }
      return JsonResponse(200, SuccessEnvelope("completed platform job", SerializeJobRecord(*job)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "job completion failed", error.what(), "PostgresError", "completeJob");
    }
  }
  PlatformJobRecord &job = jobs_[match.parameters.at("job_id")];
  if (job.job_id.empty())
  {
    return JsonResponse(404, FailureEnvelope("job completion failed", "job not found", "NotFound", "completeJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != job.worker_id)
  {
    return JsonResponse(403, FailureEnvelope("job completion failed", "worker does not own job", "WorkerError", "completeJob"));
  }
  const std::string status = request.body.value("status", "");
  if (status != "succeeded" && status != "failed" && status != "cancelled")
  {
    return JsonResponse(400, FailureEnvelope("job completion failed", "status must be succeeded, failed, or cancelled", "ValidationError", "completeJob", 2));
  }
  job.status = status;
  job.finished_at = "2026-04-24T00:02:00Z";
  if (request.body.contains("artifacts") && request.body["artifacts"].is_array())
  {
    for (const nlohmann::json &artifact : request.body["artifacts"])
    {
      job.artifacts.push_back({
          .artifact_id = artifact.value("artifact_id", "artifact"),
          .object_key = artifact.value("object_key", ""),
          .kind = artifact.value("kind", "artifact"),
      });
    }
  }
  events_[job.job_id].push_back({
      .event_id = "event-completed",
      .job_id = job.job_id,
      .status = job.status,
      .message = request.body.value("message", "job completed"),
      .created_at = job.finished_at,
  });
  return JsonResponse(200, SuccessEnvelope("completed platform job", SerializeJobRecord(job)));
}

HttpResponse PlatformRouter::HandleRegisterWorkgroupCluster(const RouteMatch &match, const HttpRequest &request)
{
  const std::string workgroup_id = match.parameters.at("workgroup_id");
  if (!config_.workgroup.enabled)
  {
    return JsonResponse(403, FailureEnvelope("workgroup registration rejected", "workgroup support is disabled", "PolicyError", "registerWorkgroupCluster", 2));
  }
  if (request.identity.has_value() && request.identity->tenant_id != config_.workgroup.registration_tenant)
  {
    return JsonResponse(403, FailureEnvelope("workgroup registration rejected", "identity tenant is not allowed to register clusters", "AuthError", "registerWorkgroupCluster", 2));
  }
  if (!config_.workgroup.registration_token.empty() &&
      request.body.value("registration_token", "") != config_.workgroup.registration_token)
  {
    return JsonResponse(403, FailureEnvelope("workgroup registration rejected", "registration token is invalid", "AuthError", "registerWorkgroupCluster", 2));
  }
  if (const std::optional<std::string> error = ValidateWorkgroupClusterRegistration(workgroup_id, request.body); error.has_value())
  {
    return JsonResponse(400, FailureEnvelope("workgroup registration rejected", *error, "ValidationError", "registerWorkgroupCluster", 2));
  }

  nlohmann::json cluster = BuildWorkgroupClusterRecord(workgroup_id, request.body, config_, request.identity);
  if (postgres_ != nullptr)
  {
    try
    {
      return JsonResponse(200, SuccessEnvelope("registered workgroup cluster", postgres_->RegisterWorkgroupCluster(workgroup_id, cluster)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "workgroup registration failed", error.what(), "PostgresError", "registerWorkgroupCluster");
    }
  }
  workgroups_[workgroup_id][cluster["cluster_id"].get<std::string>()] = cluster;
  return JsonResponse(200, SuccessEnvelope("registered workgroup cluster", cluster));
}

HttpResponse PlatformRouter::HandleListWorkgroupClusters(const RouteMatch &match) const
{
  const std::string workgroup_id = match.parameters.at("workgroup_id");
  if (!IsSafeWorkgroupId(workgroup_id))
  {
    return JsonResponse(400, FailureEnvelope("workgroup lookup rejected", "workgroup_id is invalid", "ValidationError", "listWorkgroupClusters", 2));
  }
  nlohmann::json clusters = nlohmann::json::array();
  if (postgres_ != nullptr)
  {
    try
    {
      clusters = postgres_->ListWorkgroupClusters(workgroup_id);
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "workgroup lookup failed", error.what(), "PostgresError", "listWorkgroupClusters");
    }
  }
  else if (const auto workgroup = workgroups_.find(workgroup_id); workgroup != workgroups_.end())
  {
    for (const auto &[cluster_id, cluster] : workgroup->second)
    {
      (void) cluster_id;
      clusters.push_back(cluster);
    }
  }
  return JsonResponse(
      200,
      SuccessEnvelope(
          "loaded workgroup clusters",
          {
              {"workgroup_id", workgroup_id},
              {"policy", WorkgroupPolicyPayload(config_)},
              {"clusters", clusters},
          }));
}

HttpResponse PlatformRouter::HandleMirrorStatus(const RouteMatch &match) const
{
  const std::string mirror_id = match.parameters.at("mirror_id");
  if (postgres_ != nullptr)
  {
    try
    {
      const std::optional<MirrorCursorRecord> mirror = postgres_->GetMirrorState(mirror_id);
      if (!mirror.has_value())
      {
        return FailureResponse(
            404,
            "mirror freshness unavailable",
            "mirror cursor not found",
            "MirrorError",
            "mirrorStatus");
      }
      return JsonResponse(
          200,
          SuccessEnvelope(
              "loaded mirror freshness",
              {
                  {"mirror_id", mirror->mirror_id},
                  {"region", mirror->region},
                  {"origin", mirror->origin},
                  {"freshness", mirror->freshness},
                  {"replay_cursor", mirror->replay_cursor},
              }));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "mirror freshness unavailable", error.what(), "PostgresError", "mirrorStatus");
    }
  }
  const auto mirror = mirrors_.find(mirror_id);
  if (mirror == mirrors_.end())
  {
    return FailureResponse(
        404,
        "mirror freshness unavailable",
        "mirror cursor not found",
        "MirrorError",
        "mirrorStatus");
  }
  nlohmann::json payload = {
      {"mirror_id", mirror->second.mirror_id},
      {"region", config_.region},
      {"origin", mirror->second.origin},
      {"freshness", mirror->second.freshness},
      {"replay_cursor", mirror->second.replay_cursor},
  };
  return JsonResponse(200, SuccessEnvelope("loaded mirror freshness", payload));
}

HttpResponse PlatformRouter::HandleRegistryStatus() const
{
  const fs::path registry_root(config_.registry.root);
  const fs::path key_dir(config_.registry.key_dir);
  std::error_code ec;
  if (fs::exists(registry_root, ec) && !fs::is_directory(registry_root, ec))
  {
    return FailureResponse(
        503,
        "registry status failed",
        "registry root is not a directory",
        "RegistryStatusError",
        "registryStatus");
  }
  if (fs::exists(key_dir, ec) && !fs::is_directory(key_dir, ec))
  {
    return FailureResponse(
        503,
        "registry status failed",
        "registry key directory is not a directory",
        "RegistryStatusError",
        "registryStatus");
  }

  const bool config_present = fs::exists(registry_root / "config.json", ec);
  const bool root_metadata_present = fs::exists(registry_root / "trust" / "root.json", ec);
  nlohmann::json payload = {
      {"registry_root", "<redacted>"},
      {"key_dir", "<redacted>"},
      {"registry_name", config_.registry.registry_name},
      {"root_initialized", config_present && root_metadata_present},
      {"config_present", config_present},
      {"root_metadata_present", root_metadata_present},
      {"publish_endpoint", "/api/spio-registry-control/v1/publish"},
      {"verify_endpoint", "/api/spio-registry-control/v1/verify"},
  };
  return JsonResponse(200, SuccessEnvelope("registry control plane is ready", payload));
}

HttpResponse PlatformRouter::HandlePublishRelease(const HttpRequest &request)
{
  if (!request.body.is_object())
  {
    return FailureResponse(
        400,
        "malformed registry publish request",
        "request body must be a JSON object",
        "UsageError",
        "publishRelease",
        2);
  }
  if (const std::optional<std::string> error = ValidateOptionalStringFields(
          request.body,
          {"archive_path", "manifest_path", "package", "output_path", "publisher_id", "version"}); error.has_value())
  {
    return FailureResponse(400, "malformed registry publish request", *error, "UsageError", "publishRelease", 2);
  }

  PublishDraft draft;
  try
  {
    draft = BuildPublishDraft(request.body, config_, request.identity);
  }
  catch (const std::exception &error)
  {
    return FailureResponse(
        400,
        "malformed registry publish request",
        error.what(),
        "UsageError",
        "publishRelease",
        2);
  }

  try
  {
    const fs::path registry_root(config_.registry.root);
    const std::string release_key = RegistryReleaseKey(draft.package, draft.version);
    if (published_releases_.contains(release_key) || ReleaseExistsOnDisk(registry_root, draft.package, draft.version))
    {
      return FailureResponse(
          409,
          "registry publish failed",
          "package version is already published",
          "PublishError",
          "publishRelease");
    }

    const bool created_root =
        !fs::exists(registry_root / "config.json") || !fs::exists(registry_root / "trust" / "root.json");
    EnsureRegistryRootInitialized(config_);

    const std::string archive_sha256 = spio::Sha256File(draft.archive_path);
    const uintmax_t archive_size = fs::file_size(draft.archive_path);
    const std::string artifact_path =
        "artifacts/source/sha256/" + archive_sha256.substr(0, 2) + "/" + archive_sha256.substr(2, 2) + "/" +
        archive_sha256 + ".spio.src.tar";
    fs::create_directories((registry_root / artifact_path).parent_path());
    if (!fs::exists(registry_root / artifact_path))
    {
      fs::copy_file(draft.archive_path, registry_root / artifact_path);
    }

    const size_t sequence = CountRegularFiles(registry_root / "log" / "leaves") + 1;
    const std::string log_leaf_path = "log/leaves/" + PaddedNumber(sequence, 12) + ".json";
    const std::string index_path = RegistryIndexPathForPackage(draft.package);
    const std::string published_at = "2026-04-24T00:00:00Z";

    const nlohmann::json release_record = {
        {"package", draft.package},
        {"version", draft.version},
        {"publisher_id", draft.publisher_id},
        {"published_at", published_at},
        {"archive_sha256", archive_sha256},
        {"archive_size_bytes", static_cast<int64_t>(archive_size)},
        {"artifact_path", artifact_path},
        {"sequence", static_cast<int64_t>(sequence)},
    };
    AppendJsonLine(registry_root / index_path, release_record);
    WriteJsonFileIfMissing(
        registry_root / log_leaf_path,
        {
            {"sequence", static_cast<int64_t>(sequence)},
            {"release", release_record},
        });

    nlohmann::json payload = {
        {"registry_root", registry_root.string()},
        {"created_root", created_root},
        {"package", draft.package},
        {"version", draft.version},
        {"publisher_id", draft.publisher_id},
        {"published_at", published_at},
        {"archive_path", draft.archive_path.string()},
        {"archive_sha256", archive_sha256},
        {"archive_size_bytes", static_cast<int64_t>(archive_size)},
        {"artifact_path", artifact_path},
        {"index_path", index_path},
        {"log_leaf_path", log_leaf_path},
        {"sequence", static_cast<int64_t>(sequence)},
        {"dependencies", draft.dependencies},
        {"dev_dependencies", draft.dev_dependencies},
    };
    published_releases_[release_key] = payload;
    RecordMirrorState("fresh", "checkpoint-" + PaddedNumber(sequence, 4));
    return JsonResponse(200, SuccessEnvelope("published registry v2 release", payload));
  }
  catch (const std::exception &error)
  {
    return FailureResponse(422, "registry publish failed", error.what(), "PublishError", "publishRelease");
  }
}

HttpResponse PlatformRouter::HandleVerifyRegistry(const HttpRequest &request)
{
  if (!request.body.is_object() || !request.body.empty())
  {
    return FailureResponse(
        400,
        "registry verification failed",
        "verify request must be an empty JSON object",
        "VerifyError",
        "verifyRegistry",
        2);
  }

  try
  {
    const fs::path registry_root(config_.registry.root);
    if (!fs::exists(registry_root / "config.json") || !fs::exists(registry_root / "trust" / "root.json"))
    {
      return FailureResponse(
          422,
          "registry verification failed",
          "registry root is not initialized",
          "VerifyError",
          "verifyRegistry");
    }
    const size_t namespaces = CountNamespaces(registry_root / "index");
    const size_t index_files = CountRegularFiles(registry_root / "index");
    const size_t releases = CountIndexReleases(registry_root / "index");
    const size_t tree_size = CountRegularFiles(registry_root / "log" / "leaves");
    nlohmann::json payload = {
        {"ok", true},
        {"root", registry_root.string()},
        {"namespaces", static_cast<int64_t>(namespaces)},
        {"index_files", static_cast<int64_t>(index_files)},
        {"releases", static_cast<int64_t>(releases)},
        {"tree_size", static_cast<int64_t>(tree_size)},
    };
    RecordMirrorState("fresh", "checkpoint-" + PaddedNumber(tree_size, 4));
    return JsonResponse(200, SuccessEnvelope("verified registry v2 root", payload));
  }
  catch (const std::exception &error)
  {
    return FailureResponse(422, "registry verification failed", error.what(), "VerifyError", "verifyRegistry");
  }
}

void PlatformRouter::RecordMirrorState(std::string freshness, std::string replay_cursor)
{
  if (postgres_ != nullptr)
  {
    postgres_->RecordMirrorState(
        config_.registry.mirror_id,
        config_.region,
        config_.registry.mirror_origin,
        freshness,
        replay_cursor);
  }
  mirrors_[config_.registry.mirror_id] = RegistryMirrorState{
      .mirror_id = config_.registry.mirror_id,
      .origin = config_.registry.mirror_origin,
      .freshness = std::move(freshness),
      .replay_cursor = std::move(replay_cursor),
  };
}

}  // namespace spio::platform
