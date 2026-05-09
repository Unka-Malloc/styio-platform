#include "PlatformService/Router.hpp"

#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"
#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"
#include "PlatformCore/Core/Errors.hpp"
#include "PlatformCore/Core/Process.hpp"
#include "PlatformCore/Core/Sha256.hpp"
#include "PlatformCore/Manifest/Manifest.hpp"
#include "PlatformSecurity/PlatformClientAuth/Authorization.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <tuple>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

bool IsRegistryOperation(std::string_view operation_id)
{
  static constexpr std::array<std::string_view, 23> kRegistryOperations = {
      "registryStatus",
      "registryDescriptor",
      "publishRelease",
      "verifyRegistry",
      "getPackage",
      "listPackageReleases",
      "getPackageRelease",
      "yankPackageRelease",
      "unyankPackageRelease",
      "listPackageOwners",
      "addPackageOwner",
      "removePackageOwner",
      "createPublishToken",
      "listPublishTokens",
      "revokePublishToken",
      "listRepositories",
      "listRepositoryVersions",
      "getPublication",
      "verifyPublication",
      "listDistributions",
      "promoteDistribution",
      "rollbackDistribution",
      "mirrorStatus",
  };
  return std::find(kRegistryOperations.begin(), kRegistryOperations.end(), operation_id) != kRegistryOperations.end();
}

bool IsTokenCapableRegistryOperation(std::string_view operation_id)
{
  return operation_id == "publishRelease" || operation_id == "yankPackageRelease" ||
         operation_id == "unyankPackageRelease" || operation_id == "addPackageOwner" ||
         operation_id == "removePackageOwner" || operation_id == "promoteDistribution" ||
         operation_id == "rollbackDistribution";
}

std::optional<std::string> RegistryTokenFromHeaders(const std::map<std::string, std::string> &headers)
{
  if (const auto direct = headers.find("x-styio-registry-token"); direct != headers.end() && !direct->second.empty())
  {
    return direct->second;
  }
  const auto authorization = headers.find("authorization");
  if (authorization == headers.end())
  {
    return std::nullopt;
  }
  constexpr std::string_view prefix = "Bearer ";
  if (!authorization->second.starts_with(prefix))
  {
    return std::nullopt;
  }
  return authorization->second.substr(prefix.size());
}

bool UsesPostgresState(const PlatformConfig &config)
{
  return config.state_backend == "postgres";
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

std::string FileUrlForPath(const fs::path &path)
{
  return "file://" + fs::absolute(path).lexically_normal().generic_string();
}

std::string RegistryReadRootUrl(const PlatformConfig &config)
{
  if (!config.registry.read_root_url.empty())
  {
    return config.registry.read_root_url;
  }
  if (ParseObjectStoreProvider(config.object_store.provider) == ObjectStoreProvider::S3 &&
      !config.object_store.endpoint.empty() && !config.object_store.bucket.empty() && config.object_store.path_style)
  {
    std::string root = config.object_store.endpoint;
    while (!root.empty() && root.back() == '/')
    {
      root.pop_back();
    }
    root += "/" + config.object_store.bucket;
    std::string prefix = config.object_store.prefix;
    while (!prefix.empty() && prefix.front() == '/')
    {
      prefix.erase(prefix.begin());
    }
    while (!prefix.empty() && prefix.back() == '/')
    {
      prefix.pop_back();
    }
    if (!prefix.empty())
    {
      root += "/" + prefix;
    }
    return root;
  }
  return FileUrlForPath(config.registry.root);
}

std::string RegistryControlPlaneBaseUrl(const PlatformConfig &config)
{
  if (!config.registry.control_plane_base_url.empty())
  {
    return config.registry.control_plane_base_url;
  }
  return "/api/spio-registry-control/v1";
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

std::optional<std::string> ValidateCompileContainerRegistration(const nlohmann::json &body)
{
  if (!body.is_object())
  {
    return "request body must be an object";
  }
  for (const std::string field : {"container_id", "worker_id", "tenant_id", "user_id", "workspace_id", "region", "worker_pool_key"})
  {
    if (!HasNonEmptyString(body, field))
    {
      return field + " is required";
    }
  }
  const int capacity = body.value("capacity", 0);
  if (capacity < 1)
  {
    return "capacity must be positive";
  }
  if (body.contains("status") && body["status"] != "active" && body["status"] != "draining")
  {
    return "status must be active or draining when present";
  }
  return std::nullopt;
}

std::optional<std::string> ValidateCompileContainerSwitch(const nlohmann::json &body)
{
  if (!body.is_object())
  {
    return "request body must be an object";
  }
  for (const std::string field : {"worker_id", "workspace_id"})
  {
    if (!HasNonEmptyString(body, field))
    {
      return field + " is required";
    }
  }
  if (const std::optional<std::string> error = ValidateOptionalStringFields(body, {"tenant_id", "user_id", "reason"}); error.has_value())
  {
    return error;
  }
  return std::nullopt;
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

bool VersionLess(const std::string &left, const std::string &right);
std::string CanonicalJson(const nlohmann::json &payload);
std::string Sha256Bytes(std::string_view payload);

std::string RegistryPackageId(std::string_view package)
{
  return std::string(package);
}

std::string RegistryPackageFromRoute(const RouteMatch &match)
{
  return match.parameters.at("namespace") + "/" + match.parameters.at("name");
}

std::string RegistryActorId(const HttpRequest &request)
{
  if (request.identity.has_value())
  {
    return request.identity->node_id;
  }
  if (const std::optional<std::string> token = RegistryTokenFromHeaders(request.headers); token.has_value())
  {
    const std::string hash = Sha256Bytes(*token);
    return "token:" + hash.substr(0, 12);
  }
  return "anonymous";
}

std::vector<std::string> JsonStringArray(const nlohmann::json &body, const std::string &field, std::vector<std::string> fallback)
{
  if (!body.contains(field))
  {
    return fallback;
  }
  std::vector<std::string> values;
  for (const nlohmann::json &entry : body.at(field))
  {
    values.push_back(entry.get<std::string>());
  }
  return values;
}

bool PackagePatternMatches(std::string_view pattern, std::string_view package)
{
  if (pattern == "*")
  {
    return true;
  }
  if (pattern.ends_with("/*"))
  {
    const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
    return package.starts_with(prefix);
  }
  return pattern == package;
}

bool TokenHasScope(const RegistryPublishTokenRecord &token, std::string_view scope)
{
  return std::find(token.scopes.begin(), token.scopes.end(), scope) != token.scopes.end();
}

bool TokenMatchesPackage(const RegistryPublishTokenRecord &token, const std::string &package)
{
  if (package.empty())
  {
    return true;
  }
  return std::any_of(token.package_patterns.begin(), token.package_patterns.end(), [&](const std::string &pattern) {
    return PackagePatternMatches(pattern, package);
  });
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

std::vector<nlohmann::json> ReadPackageIndexRecords(const fs::path &registry_root, const std::string &package)
{
  const fs::path index_path = registry_root / RegistryIndexPathForPackage(package);
  std::vector<nlohmann::json> records;
  std::ifstream in(index_path);
  if (!in)
  {
    return records;
  }
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty())
    {
      continue;
    }
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

void WritePackageIndexRecords(
    const fs::path &registry_root,
    const std::string &package,
    const std::vector<nlohmann::json> &records)
{
  const fs::path index_path = registry_root / RegistryIndexPathForPackage(package);
  fs::create_directories(index_path.parent_path());
  std::ofstream out(index_path, std::ios::binary | std::ios::trunc);
  for (const nlohmann::json &record : records)
  {
    out << record.dump() << "\n";
  }
}

nlohmann::json PackagePayloadFromIndex(const fs::path &registry_root, const std::string &package)
{
  const std::vector<nlohmann::json> records = ReadPackageIndexRecords(registry_root, package);
  if (records.empty())
  {
    throw std::runtime_error("package is not found");
  }
  const size_t slash = package.find('/');
  std::vector<std::string> versions;
  nlohmann::json releases = nlohmann::json::array();
  for (const nlohmann::json &record : records)
  {
    versions.push_back(record.at("version").get<std::string>());
    releases.push_back({
        {"version", record.at("version").get<std::string>()},
        {"publisher_id", record.value("publisher_id", "")},
        {"published_at", record.value("published_at", "")},
        {"yanked", record.value("yanked", false)},
        {"source_artifact_sha256", record.at("source_artifact").at("sha256").get<std::string>()},
        {"source_artifact_path", record.at("source_artifact").at("path").get<std::string>()},
    });
  }
  std::sort(versions.begin(), versions.end(), VersionLess);
  return {
      {"package_id", package},
      {"namespace", package.substr(0, slash)},
      {"name", package.substr(slash + 1)},
      {"visibility", "public"},
      {"latest_version", versions.back()},
      {"release_count", static_cast<int64_t>(records.size())},
      {"releases", releases},
  };
}

std::optional<nlohmann::json> PackageReleasePayloadFromIndex(
    const fs::path &registry_root,
    const std::string &package,
    const std::string &version)
{
  for (const nlohmann::json &record : ReadPackageIndexRecords(registry_root, package))
  {
    if (record.value("version", "") == version)
    {
      return record;
    }
  }
  return std::nullopt;
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

std::string ReadFileBytes(const fs::path &path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string DirectoryDigest(const fs::path &root)
{
  std::error_code ec;
  nlohmann::json files = nlohmann::json::array();
  if (!fs::exists(root, ec))
  {
    return "sha256:" + Sha256Bytes(CanonicalJson(files));
  }
  std::vector<fs::path> paths;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec))
  {
    if (entry.is_regular_file(ec))
    {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const fs::path &path : paths)
  {
    files.push_back({
        {"path", fs::relative(path, root).generic_string()},
        {"sha256", spio::Sha256File(path)},
    });
  }
  return "sha256:" + Sha256Bytes(CanonicalJson(files));
}

size_t NextPublicationSequenceOnDisk(const fs::path &registry_root)
{
  const fs::path publications_root = registry_root / "_publications";
  std::error_code ec;
  size_t sequence = 1;
  if (!fs::exists(publications_root, ec))
  {
    return sequence;
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(publications_root, ec))
  {
    if (!entry.is_directory(ec))
    {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.starts_with("pub-"))
    {
      try
      {
        sequence = std::max(sequence, static_cast<size_t>(std::stoul(name.substr(4)) + 1U));
      }
      catch (...)
      {
      }
    }
  }
  return sequence;
}

void CopyRegistryReadPlaneTo(const fs::path &registry_root, const fs::path &dest)
{
  fs::create_directories(dest);
  std::error_code ec;
  for (const std::string entry : {"index", "artifacts", "trust", "log"})
  {
    const fs::path source = registry_root / entry;
    if (fs::exists(source, ec))
    {
      fs::copy(source, dest / entry, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
      if (ec)
      {
        throw std::runtime_error("failed to copy registry publication tree: " + ec.message());
      }
    }
  }
  fs::copy_file(registry_root / "config.json", dest / "config.json", fs::copy_options::overwrite_existing, ec);
  if (ec)
  {
    throw std::runtime_error("failed to copy registry publication config: " + ec.message());
  }
}

void MaterializePublicationToRoot(const fs::path &registry_root, const fs::path &publication_root)
{
  std::error_code ec;
  fs::remove(registry_root / "config.json", ec);
  for (const std::string entry : {"index", "artifacts", "trust", "log"})
  {
    fs::remove_all(registry_root / entry, ec);
  }
  CopyRegistryReadPlaneTo(publication_root, registry_root);
}

std::optional<nlohmann::json> CurrentDistributionPointer(const fs::path &registry_root, const std::string &distribution_id)
{
  const fs::path pointer = registry_root / "_distributions" / distribution_id / "current.json";
  if (!fs::exists(pointer))
  {
    return std::nullopt;
  }
  return nlohmann::json::parse(ReadFileBytes(pointer));
}

void WriteTextFile(const fs::path &path, const std::string &payload)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << payload;
}

std::string CanonicalJson(const nlohmann::json &payload)
{
  return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

std::string JsonText(const nlohmann::json &payload)
{
  return payload.dump(2) + "\n";
}

std::string HexBytes(const unsigned char *data, const size_t size)
{
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t index = 0; index < size; ++index)
  {
    out << std::setw(2) << static_cast<int>(data[index]);
  }
  return out.str();
}

std::string Sha256Bytes(std::string_view payload)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::string SecureRandomHex(const size_t bytes)
{
  std::vector<unsigned char> random(bytes);
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
  {
    throw std::runtime_error("secure random generation failed");
  }
  return HexBytes(random.data(), random.size());
}

std::string Base64Encode(std::string_view payload)
{
  std::string encoded(4U * ((payload.size() + 2U) / 3U), '\0');
  const int written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char *>(encoded.data()),
      reinterpret_cast<const unsigned char *>(payload.data()),
      static_cast<int>(payload.size()));
  if (written < 0)
  {
    throw std::runtime_error("base64 encoding failed");
  }
  encoded.resize(static_cast<size_t>(written));
  return encoded;
}

std::string UtcTimestampPlusDays(const int days)
{
  const auto now = std::chrono::system_clock::now() + std::chrono::hours(24 * days);
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string UtcTimestampNow()
{
  return UtcTimestampPlusDays(0);
}

spio::ProcessResult RunRegistryOpenSsl(
    std::vector<std::string> args,
    std::string input = {},
    const size_t max_stdout_bytes = 1U << 20)
{
  spio::ProcessResult result = spio::RunProcess({
      .program = "openssl",
      .args = std::move(args),
      .timeout = std::chrono::seconds{30},
      .max_stdout_bytes = max_stdout_bytes,
      .max_stderr_bytes = 1U << 20,
      .stdin_text = std::move(input),
      .error_context = "registry v2 openssl command",
  });
  if (result.exit_code != 0 || result.timed_out)
  {
    throw std::runtime_error("registry v2 openssl command failed: " + spio::DescribeProcessFailure(result));
  }
  return result;
}

struct RegistryRoleKey
{
  std::string role;
  std::string keyid;
  fs::path private_key_path;
  fs::path public_key_path;
  std::string public_key_pem;
};

const std::vector<std::string> &RegistryRoleNames()
{
  static const std::vector<std::string> roles = {"root", "timestamp", "snapshot", "targets", "log"};
  return roles;
}

std::string RegistryFileKeyId(const fs::path &public_key_path)
{
  const spio::ProcessResult der = RunRegistryOpenSsl(
      {"pkey", "-pubin", "-in", public_key_path.string(), "-outform", "DER"},
      {},
      64U << 10);
  return Sha256Bytes(der.stdout_text);
}

void GenerateRegistryKeyDirectory(const fs::path &key_dir)
{
  fs::create_directories(key_dir / "private");
  fs::create_directories(key_dir / "public");
  nlohmann::json roles = nlohmann::json::object();
  for (const std::string &role : RegistryRoleNames())
  {
    const fs::path private_key = key_dir / "private" / (role + ".pem");
    const fs::path public_key = key_dir / "public" / (role + ".pem");
    if (!fs::exists(private_key) || !fs::exists(public_key))
    {
      RunRegistryOpenSsl({"genpkey", "-algorithm", "Ed25519", "-out", private_key.string()});
      RunRegistryOpenSsl({"pkey", "-in", private_key.string(), "-pubout", "-out", public_key.string()});
    }
    roles[role] = {
        {"keyid", RegistryFileKeyId(public_key)},
        {"private_key_path", ("private/" + role + ".pem")},
        {"public_key_path", ("public/" + role + ".pem")},
    };
  }
  WriteTextFile(
      key_dir / "keys.json",
      JsonText({
          {"schema_version", 1},
          {"algorithm", "ed25519"},
          {"roles", roles},
      }));
}

std::map<std::string, RegistryRoleKey> LoadOrCreateRegistryRoleKeys(const fs::path &key_dir)
{
  if (!fs::exists(key_dir / "keys.json"))
  {
    GenerateRegistryKeyDirectory(key_dir);
  }
  const nlohmann::json manifest = nlohmann::json::parse(ReadFileBytes(key_dir / "keys.json"));
  std::map<std::string, RegistryRoleKey> loaded;
  for (const std::string &role : RegistryRoleNames())
  {
    const nlohmann::json role_payload = manifest.at("roles").at(role);
    RegistryRoleKey key = {
        .role = role,
        .keyid = role_payload.at("keyid").get<std::string>(),
        .private_key_path = key_dir / role_payload.at("private_key_path").get<std::string>(),
        .public_key_path = key_dir / role_payload.at("public_key_path").get<std::string>(),
    };
    key.public_key_pem = ReadFileBytes(key.public_key_path);
    loaded.emplace(role, std::move(key));
  }
  return loaded;
}

nlohmann::json RegistryRoleKeysPayload(const std::map<std::string, RegistryRoleKey> &role_keys)
{
  nlohmann::json keys = nlohmann::json::object();
  for (const auto &[role, key] : role_keys)
  {
    (void) role;
    keys[key.keyid] = {
        {"keytype", "ed25519"},
        {"scheme", "ed25519"},
        {"keyval", {{"public", key.public_key_pem}}},
    };
  }
  return keys;
}

nlohmann::json RegistryRolesPolicyPayload(const std::map<std::string, RegistryRoleKey> &role_keys)
{
  nlohmann::json roles = nlohmann::json::object();
  for (const std::string &role : RegistryRoleNames())
  {
    roles[role] = {
        {"keyids", {role_keys.at(role).keyid}},
        {"threshold", 1},
    };
  }
  return roles;
}

nlohmann::json SignedRegistryPayload(
    const nlohmann::json &signed_payload,
    const RegistryRoleKey &role_key,
    const fs::path &temp_root)
{
  fs::create_directories(temp_root);
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path message_path = temp_root / (role_key.role + "-" + std::to_string(stamp) + ".json");
  const fs::path signature_path = temp_root / (role_key.role + "-" + std::to_string(stamp) + ".sig");
  WriteTextFile(message_path, CanonicalJson(signed_payload));
  try
  {
    RunRegistryOpenSsl({
        "pkeyutl",
        "-sign",
        "-inkey",
        role_key.private_key_path.string(),
        "-rawin",
        "-in",
        message_path.string(),
        "-out",
        signature_path.string(),
    });
    const std::string signature = ReadFileBytes(signature_path);
    fs::remove(message_path);
    fs::remove(signature_path);
    return {
        {"signed", signed_payload},
        {"signatures", {{
                           {"keyid", role_key.keyid},
                           {"sig", Base64Encode(signature)},
                       }}},
    };
  }
  catch (...)
  {
    std::error_code ec;
    fs::remove(message_path, ec);
    fs::remove(signature_path, ec);
    throw;
  }
}

nlohmann::json RegistryConfigPayload(const PlatformConfig &config, const std::string &generated_at)
{
  return {
      {"schema_version", 1},
      {"protocol", "spio-static-registry"},
      {"protocol_version", 2},
      {"registry_name", config.registry.registry_name},
      {"generated_at", generated_at},
      {"capabilities", {
                           {"append_only_index", true},
                           {"source_artifacts", true},
                           {"binary_artifacts", true},
                           {"transparency_log", true},
                       }},
      {"paths", {
                    {"root", "trust/root.json"},
                    {"timestamp", "trust/timestamp.json"},
                    {"snapshot", "trust/snapshot.json"},
                    {"targets_prefix", "trust/targets/"},
                    {"index_prefix", "index/"},
                    {"source_artifact_prefix", "artifacts/source/"},
                    {"binary_artifact_prefix", "artifacts/binary/"},
                    {"transparency_checkpoint", "log/checkpoint.json"},
                    {"transparency_leaves_prefix", "log/leaves/"},
                }},
  };
}

nlohmann::json RegistryConfigPayload(
    const PlatformConfig &config,
    const std::string &generated_at,
    const std::string &publication_id,
    const std::string &repository_version_id)
{
  nlohmann::json payload = RegistryConfigPayload(config, generated_at);
  payload["repository_id"] = "default";
  payload["distribution_id"] = "default";
  payload["publication"] = {
      {"publication_id", publication_id},
      {"repository_version_id", repository_version_id},
      {"layout_version", 2},
      {"publication_path", "_publications/" + publication_id + "/publication.json"},
      {"current_pointer", "_distributions/default/current.json"},
  };
  return payload;
}

nlohmann::json SignedFileMeta(const fs::path &path, const int version)
{
  return {
      {"version", version},
      {"length", static_cast<int64_t>(fs::file_size(path))},
      {"hashes", {{"sha256", spio::Sha256File(path)}}},
  };
}

int ReadSignedVersion(const fs::path &path)
{
  if (!fs::exists(path))
  {
    return 0;
  }
  try
  {
    return nlohmann::json::parse(ReadFileBytes(path)).at("signed").value("version", 0);
  }
  catch (...)
  {
    return 0;
  }
}

std::vector<fs::path> LeafSequencePaths(const fs::path &root)
{
  std::vector<fs::path> paths;
  const fs::path leaves_root = root / "log" / "leaves";
  std::error_code ec;
  if (!fs::exists(leaves_root, ec))
  {
    return paths;
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(leaves_root, ec))
  {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
    {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string TransparencyRootHash(const std::vector<std::string> &leaf_hashes)
{
  std::string state(32, '\0');
  for (const std::string &leaf_hash : leaf_hashes)
  {
    std::string leaf_bytes;
    leaf_bytes.reserve(32);
    for (size_t index = 0; index + 1 < leaf_hash.size(); index += 2)
    {
      leaf_bytes.push_back(static_cast<char>(std::stoi(leaf_hash.substr(index, 2), nullptr, 16)));
    }
    const std::string combined = state + leaf_bytes;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(combined.data()), combined.size(), digest);
    state.assign(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH);
  }
  return HexBytes(reinterpret_cast<const unsigned char *>(state.data()), state.size());
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

bool VersionLess(const std::string &left, const std::string &right)
{
  auto parse = [](const std::string &version) {
    std::array<int, 3> numeric = {0, 0, 0};
    std::string suffix;
    std::stringstream stream(version);
    std::string part;
    size_t index = 0;
    while (std::getline(stream, part, '.') && index < numeric.size())
    {
      try
      {
        numeric[index] = std::stoi(part);
      }
      catch (...)
      {
        suffix = version;
        break;
      }
      ++index;
    }
    if (index != numeric.size() || stream.good())
    {
      suffix = version;
    }
    return std::tuple<int, int, int, std::string>(numeric[0], numeric[1], numeric[2], suffix);
  };
  return parse(left) < parse(right);
}

nlohmann::json BuildReleaseRecord(
    const PublishDraft &draft,
    const std::string &published_at,
    const std::string &archive_sha256,
    const uintmax_t archive_size,
    const std::string &artifact_path)
{
  const nlohmann::json dependencies = nlohmann::json::array();
  const nlohmann::json dev_dependencies = nlohmann::json::array();
  const nlohmann::json metadata_source = {
      {"package", draft.package},
      {"version", draft.version},
      {"publisher_id", draft.publisher_id},
      {"published_at", published_at},
      {"archive_sha256", archive_sha256},
      {"dependencies", dependencies},
      {"dev_dependencies", dev_dependencies},
  };
  return {
      {"schema_version", 1},
      {"package", draft.package},
      {"version", draft.version},
      {"release_revision", 1},
      {"published_at", published_at},
      {"publisher_id", draft.publisher_id},
      {"yanked", false},
      {"deprecated_message", ""},
      {"source_artifact", {
                              {"sha256", archive_sha256},
                              {"size_bytes", static_cast<int64_t>(archive_size)},
                              {"path", artifact_path},
                              {"archive_format", "tar"},
                              {"compression", "none"},
                          }},
      {"binary_artifacts", nlohmann::json::array()},
      {"dependencies", dependencies},
      {"dev_dependencies", dev_dependencies},
      {"features", {
                       {"default", nlohmann::json::array()},
                       {"optional", nlohmann::json::array()},
                   }},
      {"manifest_digest", Sha256Bytes(draft.package + "@" + draft.version)},
      {"metadata_digest", Sha256Bytes(CanonicalJson(metadata_source))},
  };
}

struct LocalAppendResult
{
  std::string index_path;
  std::string log_leaf_path;
  size_t sequence = 0;
};

LocalAppendResult AppendRegistryReleaseToLocal(
    const PlatformConfig &config,
    const PublishDraft &draft,
    const nlohmann::json &release_record,
    const std::string &artifact_path)
{
  const fs::path registry_root(config.registry.root);
  const fs::path artifact_dest_path = registry_root / artifact_path;
  fs::create_directories(artifact_dest_path.parent_path());
  if (fs::exists(artifact_dest_path))
  {
    if (spio::Sha256File(artifact_dest_path) != release_record.at("source_artifact").at("sha256").get<std::string>())
    {
      throw std::runtime_error("destination artifact already exists with different content");
    }
  }
  else
  {
    fs::copy_file(draft.archive_path, artifact_dest_path);
  }

  const std::string index_path = RegistryIndexPathForPackage(draft.package);
  const fs::path index_file = registry_root / index_path;
  if (fs::exists(index_file))
  {
    std::ifstream in(index_file);
    std::string line;
    while (std::getline(in, line))
    {
      if (JsonLineHasRelease(line, draft.package, draft.version))
      {
        throw std::runtime_error("package version is already published");
      }
    }
  }
  AppendJsonLine(index_file, release_record);

  const size_t sequence = LeafSequencePaths(registry_root).size() + 1;
  const std::string log_leaf_path = "log/leaves/" + PaddedNumber(sequence, 12) + ".json";
  const std::string package_namespace = SplitPackageName(draft.package).front();
  const nlohmann::json leaf = {
      {"schema_version", 1},
      {"sequence", static_cast<int64_t>(sequence)},
      {"namespace", package_namespace},
      {"package", draft.package},
      {"version", draft.version},
      {"release_revision", 1},
      {"index_path", index_path},
      {"index_record_sha256", Sha256Bytes(CanonicalJson(release_record))},
      {"source_artifact_sha256", release_record.at("source_artifact").at("sha256").get<std::string>()},
      {"source_artifact_path", artifact_path},
  };
  WriteTextFile(registry_root / log_leaf_path, JsonText(leaf));
  return {.index_path = index_path, .log_leaf_path = log_leaf_path, .sequence = sequence};
}

struct PackageMaps
{
  nlohmann::json namespace_packages = nlohmann::json::object();
  nlohmann::json snapshot_meta = nlohmann::json::object();
};

PackageMaps CollectPackageMaps(const fs::path &registry_root)
{
  PackageMaps result;
  const fs::path index_root = registry_root / "index";
  std::error_code ec;
  if (!fs::exists(index_root, ec))
  {
    return result;
  }
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(index_root, ec))
  {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".jsonl")
    {
      continue;
    }
    std::ifstream in(entry.path());
    std::vector<nlohmann::json> records;
    std::string line;
    while (std::getline(in, line))
    {
      if (!line.empty())
      {
        records.push_back(nlohmann::json::parse(line));
      }
    }
    if (records.empty())
    {
      throw std::runtime_error("registry index file is empty: " + entry.path().string());
    }
    const std::string package_name = records.front().at("package").get<std::string>();
    const std::string package_namespace = SplitPackageName(package_name).front();
    std::vector<std::string> versions;
    nlohmann::json releases = nlohmann::json::object();
    for (const nlohmann::json &record : records)
    {
      if (record.at("package").get<std::string>() != package_name)
      {
        throw std::runtime_error("registry index file contains mixed package names: " + entry.path().string());
      }
      const std::string version = record.at("version").get<std::string>();
      const nlohmann::json source_artifact = record.at("source_artifact");
      versions.push_back(version);
      releases[version] = {
          {"release_revision", record.value("release_revision", 1)},
          {"index_record_sha256", Sha256Bytes(CanonicalJson(record))},
          {"source_artifact_sha256", source_artifact.at("sha256").get<std::string>()},
          {"source_artifact_path", source_artifact.at("path").get<std::string>()},
          {"binary_artifact_count", record.value("binary_artifacts", nlohmann::json::array()).size()},
      };
    }
    std::sort(versions.begin(), versions.end(), VersionLess);
    const std::string relative = fs::relative(entry.path(), registry_root).generic_string();
    result.namespace_packages[package_namespace][package_name] = {
        {"index_path", relative},
        {"latest_version", versions.back()},
        {"releases", releases},
    };
    result.snapshot_meta[relative] = SignedFileMeta(entry.path(), 1);
  }
  return result;
}

struct MetadataVersions
{
  int checkpoint_version = 0;
  int snapshot_version = 0;
  int timestamp_version = 0;
  size_t namespaces = 0;
};

MetadataVersions RefreshSignedRegistryMetadata(
    const PlatformConfig &config,
    const std::map<std::string, RegistryRoleKey> &role_keys,
    const std::string &registry_time)
{
  const fs::path registry_root(config.registry.root);
  const fs::path temp_root = registry_root / "_tmp";
  PackageMaps package_maps = CollectPackageMaps(registry_root);
  for (auto &[namespace_name, package_map] : package_maps.namespace_packages.items())
  {
    const fs::path targets_path = registry_root / "trust" / "targets" / (namespace_name + ".json");
    const int targets_version = ReadSignedVersion(targets_path) + 1;
    const nlohmann::json targets_signed = {
        {"type", "targets"},
        {"spec_version", "1"},
        {"version", targets_version},
        {"expires", UtcTimestampPlusDays(30)},
        {"namespace", namespace_name},
        {"packages", package_map},
    };
    WriteTextFile(targets_path, JsonText(SignedRegistryPayload(targets_signed, role_keys.at("targets"), temp_root)));
    package_maps.snapshot_meta[fs::relative(targets_path, registry_root).generic_string()] =
        SignedFileMeta(targets_path, targets_version);
  }

  std::vector<std::string> leaf_hashes;
  for (const fs::path &leaf_path : LeafSequencePaths(registry_root))
  {
    leaf_hashes.push_back(Sha256Bytes(CanonicalJson(nlohmann::json::parse(ReadFileBytes(leaf_path)))));
  }
  const fs::path checkpoint_path = registry_root / "log" / "checkpoint.json";
  const int checkpoint_version = ReadSignedVersion(checkpoint_path) + 1;
  const nlohmann::json checkpoint_signed = {
      {"type", "checkpoint"},
      {"spec_version", "1"},
      {"version", checkpoint_version},
      {"generated_at", registry_time},
      {"tree_size", static_cast<int64_t>(leaf_hashes.size())},
      {"root_hash", TransparencyRootHash(leaf_hashes)},
  };
  WriteTextFile(checkpoint_path, JsonText(SignedRegistryPayload(checkpoint_signed, role_keys.at("log"), temp_root)));

  const fs::path snapshot_path = registry_root / "trust" / "snapshot.json";
  const int snapshot_version = ReadSignedVersion(snapshot_path) + 1;
  const nlohmann::json snapshot_signed = {
      {"type", "snapshot"},
      {"spec_version", "1"},
      {"version", snapshot_version},
      {"expires", UtcTimestampPlusDays(7)},
      {"meta", package_maps.snapshot_meta},
      {"log_meta", {{"log/checkpoint.json", SignedFileMeta(checkpoint_path, checkpoint_version)}}},
  };
  WriteTextFile(snapshot_path, JsonText(SignedRegistryPayload(snapshot_signed, role_keys.at("snapshot"), temp_root)));

  const fs::path timestamp_path = registry_root / "trust" / "timestamp.json";
  const int timestamp_version = ReadSignedVersion(timestamp_path) + 1;
  const nlohmann::json timestamp_signed = {
      {"type", "timestamp"},
      {"spec_version", "1"},
      {"version", timestamp_version},
      {"expires", UtcTimestampPlusDays(1)},
      {"meta", {{"trust/snapshot.json", SignedFileMeta(snapshot_path, snapshot_version)}}},
  };
  WriteTextFile(timestamp_path, JsonText(SignedRegistryPayload(timestamp_signed, role_keys.at("timestamp"), temp_root)));
  std::error_code ec;
  fs::remove_all(temp_root, ec);

  return {
      .checkpoint_version = checkpoint_version,
      .snapshot_version = snapshot_version,
      .timestamp_version = timestamp_version,
      .namespaces = package_maps.namespace_packages.size(),
  };
}

void EnsureRegistryRootInitialized(const PlatformConfig &config)
{
  const fs::path root(config.registry.root);
  fs::create_directories(root);
  const fs::path key_dir(config.registry.key_dir);
  fs::create_directories(key_dir);
  const std::map<std::string, RegistryRoleKey> role_keys = LoadOrCreateRegistryRoleKeys(key_dir);
  const fs::path temp_root = root / "_tmp";
  const std::string registry_time = UtcTimestampNow();

  if (!fs::exists(root / "config.json"))
  {
    WriteTextFile(root / "config.json", JsonText(RegistryConfigPayload(config, registry_time)));
  }
  if (!fs::exists(root / "log" / "checkpoint.json"))
  {
    const nlohmann::json checkpoint_signed = {
        {"type", "checkpoint"},
        {"spec_version", "1"},
        {"version", 1},
        {"generated_at", registry_time},
        {"tree_size", 0},
        {"root_hash", TransparencyRootHash({})},
    };
    WriteTextFile(root / "log" / "checkpoint.json", JsonText(SignedRegistryPayload(checkpoint_signed, role_keys.at("log"), temp_root)));
  }
  if (!fs::exists(root / "trust" / "snapshot.json"))
  {
    const nlohmann::json snapshot_signed = {
        {"type", "snapshot"},
        {"spec_version", "1"},
        {"version", 1},
        {"expires", UtcTimestampPlusDays(7)},
        {"meta", nlohmann::json::object()},
        {"log_meta", {{"log/checkpoint.json", SignedFileMeta(root / "log" / "checkpoint.json", 1)}}},
    };
    WriteTextFile(root / "trust" / "snapshot.json", JsonText(SignedRegistryPayload(snapshot_signed, role_keys.at("snapshot"), temp_root)));
  }
  if (!fs::exists(root / "trust" / "timestamp.json"))
  {
    const nlohmann::json timestamp_signed = {
        {"type", "timestamp"},
        {"spec_version", "1"},
        {"version", 1},
        {"expires", UtcTimestampPlusDays(1)},
        {"meta", {{"trust/snapshot.json", SignedFileMeta(root / "trust" / "snapshot.json", 1)}}},
    };
    WriteTextFile(root / "trust" / "timestamp.json", JsonText(SignedRegistryPayload(timestamp_signed, role_keys.at("timestamp"), temp_root)));
  }
  if (!fs::exists(root / "trust" / "root.json"))
  {
    const nlohmann::json root_signed = {
        {"type", "root"},
        {"spec_version", "1"},
        {"version", 1},
        {"expires", UtcTimestampPlusDays(365)},
        {"keys", RegistryRoleKeysPayload(role_keys)},
        {"roles", RegistryRolesPolicyPayload(role_keys)},
    };
    WriteTextFile(root / "trust" / "root.json", JsonText(SignedRegistryPayload(root_signed, role_keys.at("root"), temp_root)));
  }
  std::error_code ec;
  fs::remove_all(temp_root, ec);
}

bool UsesS3ObjectStore(const PlatformConfig &config)
{
  return ParseObjectStoreProvider(config.object_store.provider) == ObjectStoreProvider::S3;
}

void RemoveLocalRegistryMetadataCache(const PlatformConfig &config)
{
  const fs::path root(config.registry.root);
  std::error_code ec;
  fs::remove(root / "config.json", ec);
  fs::remove_all(root / "trust", ec);
  fs::remove_all(root / "index", ec);
  fs::remove_all(root / "log", ec);
  fs::remove_all(root / "artifacts", ec);
}

void SyncS3RegistryStateToLocal(const PlatformConfig &config)
{
  RemoveLocalRegistryMetadataCache(config);
  const fs::path root(config.registry.root);
  if (const std::optional<std::string> config_text = GetObjectText(config.object_store, "config.json"); config_text.has_value())
  {
    WriteTextFile(root / "config.json", *config_text);
  }
  for (const std::string &prefix : {"trust/", "index/", "log/", "artifacts/", "_publications/", "_distributions/"})
  {
    for (const std::string &key : ListObjectKeys(config.object_store, prefix))
    {
      if (key.starts_with("_staging/") || key.starts_with("_tmp/"))
      {
        continue;
      }
      if (const std::optional<std::string> payload = GetObjectText(config.object_store, key); payload.has_value())
      {
        WriteTextFile(root / NormalizeObjectKey(key), *payload);
      }
    }
  }
}

std::string RegistryContentTypeForPath(const std::string &relative_path)
{
  if (relative_path.ends_with(".json"))
  {
    return "application/json";
  }
  if (relative_path.ends_with(".jsonl"))
  {
    return "application/x-ndjson";
  }
  return "application/octet-stream";
}

void UploadRegistryTreeToS3(const PlatformConfig &config)
{
  const fs::path root(config.registry.root);
  std::error_code ec;
  if (!fs::exists(root, ec))
  {
    return;
  }
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec))
  {
    if (!entry.is_regular_file(ec))
    {
      continue;
    }
    const std::string relative = fs::relative(entry.path(), root).generic_string();
    if (relative.starts_with("_staging/") || relative.starts_with("_tmp/"))
    {
      continue;
    }
    PutObjectFile(config.object_store, relative, entry.path(), RegistryContentTypeForPath(relative));
  }
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
      {.operation_id = "registerCompileContainer", .method = HttpMethod::Post, .path = "/compile-containers/register", .internal = true},
      {.operation_id = "getCompileContainer", .method = HttpMethod::Get, .path = "/compile-containers/{container_id}", .internal = true},
      {.operation_id = "switchCompileContainerWorkspace", .method = HttpMethod::Post, .path = "/compile-containers/{container_id}/switch-workspace", .internal = true},
      {.operation_id = "claimJob", .method = HttpMethod::Post, .path = "/jobs/claim", .internal = true},
      {.operation_id = "heartbeatJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/heartbeat", .internal = true},
      {.operation_id = "completeJob", .method = HttpMethod::Post, .path = "/jobs/{job_id}/complete", .internal = true},
      {.operation_id = "registerWorkgroupCluster", .method = HttpMethod::Post, .path = "/workgroups/{workgroup_id}/clusters/register", .internal = true},
      {.operation_id = "listWorkgroupClusters", .method = HttpMethod::Get, .path = "/workgroups/{workgroup_id}/clusters", .internal = true},
      {.operation_id = "mirrorStatus", .method = HttpMethod::Get, .path = "/mirrors/{mirror_id}/status"},
  };
}

PlatformRouter::PlatformRouter(PlatformConfig config)
    : config_(std::move(config)), routes_(BuildPlatformControlPlaneRoutes())
{
  const std::vector<RouteSpec> registry_routes = BuildRegistryControlPlaneRoutes();
  routes_.insert(routes_.end(), registry_routes.begin(), registry_routes.end());
  memory_.RecordMirrorState(
      config_.registry.mirror_id,
      config_.region,
      config_.registry.mirror_origin,
      "lagging",
      "checkpoint-0000");
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
  if (operation == "registerCompileContainer")
  {
    return HandleRegisterCompileContainer(request);
  }
  if (operation == "getCompileContainer")
  {
    return HandleGetCompileContainer(*match);
  }
  if (operation == "switchCompileContainerWorkspace")
  {
    return HandleSwitchCompileContainerWorkspace(*match, request);
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
  if (operation == "registryDescriptor")
  {
    return HandleRegistryDescriptor();
  }
  if (operation == "publishRelease")
  {
    return HandlePublishRelease(request);
  }
  if (operation == "verifyRegistry")
  {
    return HandleVerifyRegistry(request);
  }
  if (operation == "getPackage")
  {
    return HandleGetPackage(*match);
  }
  if (operation == "listPackageReleases")
  {
    return HandleListPackageReleases(*match);
  }
  if (operation == "getPackageRelease")
  {
    return HandleGetPackageRelease(*match);
  }
  if (operation == "yankPackageRelease")
  {
    return HandleSetPackageReleaseYanked(*match, request, true);
  }
  if (operation == "unyankPackageRelease")
  {
    return HandleSetPackageReleaseYanked(*match, request, false);
  }
  if (operation == "listPackageOwners")
  {
    return HandleListPackageOwners(*match);
  }
  if (operation == "addPackageOwner")
  {
    return HandleAddPackageOwner(*match, request);
  }
  if (operation == "removePackageOwner")
  {
    return HandleRemovePackageOwner(*match, request);
  }
  if (operation == "createPublishToken")
  {
    return HandleCreatePublishToken(request);
  }
  if (operation == "listPublishTokens")
  {
    return HandleListPublishTokens(request);
  }
  if (operation == "revokePublishToken")
  {
    return HandleRevokePublishToken(*match, request);
  }
  if (operation == "listRepositories")
  {
    return HandleListRepositories();
  }
  if (operation == "listRepositoryVersions")
  {
    return HandleListRepositoryVersions(*match);
  }
  if (operation == "getPublication")
  {
    return HandleGetPublication(*match);
  }
  if (operation == "verifyPublication")
  {
    return HandleVerifyPublication(*match, request);
  }
  if (operation == "listDistributions")
  {
    return HandleListDistributions();
  }
  if (operation == "promoteDistribution")
  {
    return HandlePromoteDistribution(*match, request);
  }
  if (operation == "rollbackDistribution")
  {
    return HandleRollbackDistribution(*match, request);
  }
  return JsonResponse(500, FailureEnvelope("route handler missing", operation, "InternalError", operation));
}

HttpResponse PlatformRouter::RequireIdentity(const RouteMatch &match, const HttpRequest &request) const
{
  if (!config_.mtls.required)
  {
    return JsonResponse(200, {});
  }
  if (!request.identity.has_value() && match.route.internal &&
      IsTokenCapableRegistryOperation(match.route.operation_id) &&
      RegistryTokenFromHeaders(request.headers).has_value())
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
  std::string job_id;
  if (postgres_ != nullptr)
  {
    try
    {
      job_id = postgres_->NextJobId();
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "job submission failed", error.what(), "PostgresError", "submitJob");
    }
  }
  else
  {
    job_id = memory_.NextJobId();
  }
  PlatformJobRecord job = BuildQueuedJobRecord(request.body, config_, std::move(job_id));
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
  memory_.SubmitJob(job);
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
  const std::optional<PlatformJobRecord> job = memory_.GetJob(match.parameters.at("job_id"));
  if (!job.has_value())
  {
    return JsonResponse(404, FailureEnvelope("job lookup failed", "job not found", "NotFound", "getJob"));
  }
  return JsonResponse(200, SuccessEnvelope("loaded platform job", SerializeJobRecord(*job)));
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
  if (!memory_.GetJob(job_id).has_value())
  {
    return JsonResponse(404, FailureEnvelope("job event lookup failed", "job not found", "NotFound", "getJobEvents"));
  }
  nlohmann::json events = nlohmann::json::array();
  for (const JobEventRecord &event : memory_.GetJobEvents(job_id))
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
  const std::optional<PlatformJobRecord> job =
      memory_.CancelJob(match.parameters.at("job_id"), request.body["reason"].get<std::string>());
  if (!job.has_value())
  {
    return JsonResponse(404, FailureEnvelope("job cancellation failed", "job not found", "NotFound", "cancelJob"));
  }
  return JsonResponse(200, SuccessEnvelope("cancelled platform job", SerializeJobRecord(*job)));
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
  return JsonResponse(200, SuccessEnvelope("registered platform worker", memory_.RegisterWorker(worker)));
}

HttpResponse PlatformRouter::HandleRegisterCompileContainer(const HttpRequest &request)
{
  if (const std::optional<std::string> error = ValidateCompileContainerRegistration(request.body); error.has_value())
  {
    return JsonResponse(400, FailureEnvelope("compile container registration rejected", *error, "ValidationError", "registerCompileContainer", 2));
  }
  const CompileContainerRecordFactory container_factory;
  CompileContainerRecord container = container_factory.CreateFromRegistration(request.body);
  if (postgres_ != nullptr)
  {
    try
    {
      return JsonResponse(200, SuccessEnvelope("registered compile container", SerializeCompileContainerRecord(postgres_->RegisterCompileContainer(container))));
    }
    catch (const PostgresStoreError &error)
    {
      const std::string detail = error.what();
      if (detail.find("worker is not registered") != std::string::npos)
      {
        return JsonResponse(403, FailureEnvelope("compile container registration rejected", "worker is not registered", "WorkerError", "registerCompileContainer"));
      }
      if (detail.find("user binding mismatch") != std::string::npos)
      {
        return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container user binding mismatch", "BindingError", "registerCompileContainer", 2));
      }
      if (detail.find("worker owner mismatch") != std::string::npos)
      {
        return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container worker owner mismatch", "BindingError", "registerCompileContainer", 2));
      }
      return FailureResponse(503, "compile container registration failed", detail, "PostgresError", "registerCompileContainer");
    }
  }

  try
  {
    return JsonResponse(200, SuccessEnvelope("registered compile container", SerializeCompileContainerRecord(memory_.RegisterCompileContainer(container))));
  }
  catch (const MemoryStateStoreError &error)
  {
    const std::string detail = error.what();
    if (detail.find("worker is not registered") != std::string::npos ||
        detail.find("worker region or pool") != std::string::npos)
    {
      return JsonResponse(403, FailureEnvelope("compile container registration rejected", detail, "WorkerError", "registerCompileContainer"));
    }
    if (detail.find("user binding mismatch") != std::string::npos)
    {
      return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container user binding mismatch", "BindingError", "registerCompileContainer", 2));
    }
    if (detail.find("worker owner mismatch") != std::string::npos)
    {
      return JsonResponse(409, FailureEnvelope("compile container registration rejected", "compile container worker owner mismatch", "BindingError", "registerCompileContainer", 2));
    }
    return FailureResponse(503, "compile container registration failed", detail, "MemoryStateError", "registerCompileContainer");
  }
}

HttpResponse PlatformRouter::HandleGetCompileContainer(const RouteMatch &match) const
{
  const std::string container_id = match.parameters.at("container_id");
  if (postgres_ != nullptr)
  {
    try
    {
      const std::optional<CompileContainerRecord> container = postgres_->GetCompileContainer(container_id);
      if (!container.has_value())
      {
        return JsonResponse(404, FailureEnvelope("compile container lookup failed", "compile container not found", "NotFound", "getCompileContainer"));
      }
      return JsonResponse(200, SuccessEnvelope("loaded compile container", SerializeCompileContainerRecord(*container)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "compile container lookup failed", error.what(), "PostgresError", "getCompileContainer");
    }
  }
  const std::optional<CompileContainerRecord> container = memory_.GetCompileContainer(container_id);
  if (!container.has_value())
  {
    return JsonResponse(404, FailureEnvelope("compile container lookup failed", "compile container not found", "NotFound", "getCompileContainer"));
  }
  return JsonResponse(200, SuccessEnvelope("loaded compile container", SerializeCompileContainerRecord(*container)));
}

HttpResponse PlatformRouter::HandleSwitchCompileContainerWorkspace(const RouteMatch &match, const HttpRequest &request)
{
  if (const std::optional<std::string> error = ValidateCompileContainerSwitch(request.body); error.has_value())
  {
    return JsonResponse(400, FailureEnvelope("compile container workspace switch rejected", *error, "ValidationError", "switchCompileContainerWorkspace", 2));
  }
  const std::string container_id = match.parameters.at("container_id");
  const std::string worker_id = request.body["worker_id"].get<std::string>();
  const std::string workspace_id = request.body["workspace_id"].get<std::string>();
  const std::string reason = request.body.value("reason", "manual switch");

  if (postgres_ != nullptr)
  {
    try
    {
      const std::optional<CompileContainerRecord> current = postgres_->GetCompileContainer(container_id);
      if (!current.has_value())
      {
        return JsonResponse(404, FailureEnvelope("compile container workspace switch failed", "compile container not found", "NotFound", "switchCompileContainerWorkspace"));
      }
      if (current->worker_id != worker_id)
      {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "worker does not own compile container", "WorkerError", "switchCompileContainerWorkspace"));
      }
      if (request.body.contains("tenant_id") && request.body["tenant_id"].get<std::string>() != current->tenant_id)
      {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "tenant binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
      }
      if (request.body.contains("user_id") && request.body["user_id"].get<std::string>() != current->user_id)
      {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "user binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
      }
      const std::optional<CompileContainerRecord> switched =
          postgres_->SwitchCompileContainerWorkspace(container_id, worker_id, workspace_id, reason);
      if (!switched.has_value())
      {
        return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "compile container is not active", "StateError", "switchCompileContainerWorkspace"));
      }
      return JsonResponse(200, SuccessEnvelope("switched compile container workspace", SerializeCompileContainerRecord(*switched)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "compile container workspace switch failed", error.what(), "PostgresError", "switchCompileContainerWorkspace");
    }
  }

  const std::optional<CompileContainerRecord> current = memory_.GetCompileContainer(container_id);
  if (!current.has_value())
  {
    return JsonResponse(404, FailureEnvelope("compile container workspace switch failed", "compile container not found", "NotFound", "switchCompileContainerWorkspace"));
  }
  if (current->worker_id != worker_id)
  {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "worker does not own compile container", "WorkerError", "switchCompileContainerWorkspace"));
  }
  if (request.body.contains("tenant_id") && request.body["tenant_id"].get<std::string>() != current->tenant_id)
  {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "tenant binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
  }
  if (request.body.contains("user_id") && request.body["user_id"].get<std::string>() != current->user_id)
  {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "user binding mismatch", "BindingError", "switchCompileContainerWorkspace", 2));
  }
  try
  {
    const std::optional<CompileContainerRecord> switched =
        memory_.SwitchCompileContainerWorkspace(container_id, worker_id, workspace_id, reason);
    if (!switched.has_value())
    {
      return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", "compile container is not active", "StateError", "switchCompileContainerWorkspace"));
    }
    return JsonResponse(200, SuccessEnvelope("switched compile container workspace", SerializeCompileContainerRecord(*switched)));
  }
  catch (const MemoryStateStoreError &error)
  {
    return JsonResponse(403, FailureEnvelope("compile container workspace switch rejected", error.what(), "WorkerError", "switchCompileContainerWorkspace"));
  }
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
  std::optional<std::string> compile_container_id;
  if (request.body.contains("compile_container_id"))
  {
    if (!request.body["compile_container_id"].is_string() || request.body["compile_container_id"].get<std::string>().empty())
    {
      return JsonResponse(400, FailureEnvelope("job claim failed", "compile_container_id must be a non-empty string when present", "ValidationError", "claimJob", 2));
    }
    compile_container_id = request.body["compile_container_id"].get<std::string>();
  }
  if (postgres_ != nullptr)
  {
    try
    {
      const std::optional<PlatformJobRecord> job = compile_container_id.has_value()
                                                       ? postgres_->ClaimJobForCompileContainer(worker_id, region, worker_pool_key, *compile_container_id)
                                                       : postgres_->ClaimJob(worker_id, region, worker_pool_key);
      if (!job.has_value())
      {
        return JsonResponse(200, SuccessEnvelope("no platform job available", {{"claimed", false}}));
      }
      nlohmann::json payload = {{"claimed", true}, {"job", SerializeJobRecord(*job)}};
      if (compile_container_id.has_value())
      {
        if (const std::optional<CompileContainerRecord> container = postgres_->GetCompileContainer(*compile_container_id); container.has_value())
        {
          payload["compile_container"] = SerializeCompileContainerRecord(*container);
        }
      }
      return JsonResponse(200, SuccessEnvelope("claimed platform job", std::move(payload)));
    }
    catch (const PostgresStoreError &error)
    {
      const std::string detail = error.what();
      if (detail.find("worker is not registered") != std::string::npos)
      {
        return JsonResponse(403, FailureEnvelope("job claim failed", "worker is not registered", "WorkerError", "claimJob"));
      }
      if (detail.find("compile container is not registered") != std::string::npos)
      {
        return JsonResponse(403, FailureEnvelope("job claim failed", "compile container is not registered", "WorkerError", "claimJob"));
      }
      return FailureResponse(503, "job claim failed", detail, "PostgresError", "claimJob");
    }
  }
  try
  {
    const std::optional<PlatformJobRecord> job = compile_container_id.has_value()
                                                     ? memory_.ClaimJobForCompileContainer(worker_id, region, worker_pool_key, *compile_container_id)
                                                     : memory_.ClaimJob(worker_id, region, worker_pool_key);
    if (!job.has_value())
    {
      return JsonResponse(200, SuccessEnvelope("no platform job available", {{"claimed", false}}));
    }
    nlohmann::json payload = {{"claimed", true}, {"job", SerializeJobRecord(*job)}};
    if (compile_container_id.has_value())
    {
      if (const std::optional<CompileContainerRecord> container = memory_.GetCompileContainer(*compile_container_id); container.has_value())
      {
        payload["compile_container"] = SerializeCompileContainerRecord(*container);
      }
    }
    return JsonResponse(200, SuccessEnvelope("claimed platform job", std::move(payload)));
  }
  catch (const MemoryStateStoreError &error)
  {
    const std::string detail = error.what();
    if (detail.find("worker is not registered") != std::string::npos)
    {
      return JsonResponse(403, FailureEnvelope("job claim failed", "worker is not registered", "WorkerError", "claimJob"));
    }
    if (detail.find("compile container is not registered") != std::string::npos)
    {
      return JsonResponse(403, FailureEnvelope("job claim failed", "compile container is not registered", "WorkerError", "claimJob"));
    }
    if (detail.find("compile container is not available") != std::string::npos)
    {
      return JsonResponse(403, FailureEnvelope("job claim failed", "compile container is not available for this worker", "WorkerError", "claimJob"));
    }
    return FailureResponse(503, "job claim failed", detail, "MemoryStateError", "claimJob");
  }
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
  const std::optional<PlatformJobRecord> existing = memory_.GetJob(match.parameters.at("job_id"));
  if (!existing.has_value())
  {
    return JsonResponse(404, FailureEnvelope("job heartbeat failed", "job not found", "NotFound", "heartbeatJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != existing->worker_id)
  {
    return JsonResponse(403, FailureEnvelope("job heartbeat failed", "worker does not own job", "WorkerError", "heartbeatJob"));
  }
  const std::optional<PlatformJobRecord> job = memory_.HeartbeatJob(
      match.parameters.at("job_id"),
      request.body["worker_id"].get<std::string>(),
      request.body.value("message", "worker heartbeat"));
  return JsonResponse(200, SuccessEnvelope("recorded platform job heartbeat", SerializeJobRecord(*job)));
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
  const std::optional<PlatformJobRecord> existing = memory_.GetJob(match.parameters.at("job_id"));
  if (!existing.has_value())
  {
    return JsonResponse(404, FailureEnvelope("job completion failed", "job not found", "NotFound", "completeJob"));
  }
  if (!request.body.contains("worker_id") || request.body["worker_id"] != existing->worker_id)
  {
    return JsonResponse(403, FailureEnvelope("job completion failed", "worker does not own job", "WorkerError", "completeJob"));
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
  const std::optional<PlatformJobRecord> job = memory_.CompleteJob(
      match.parameters.at("job_id"),
      request.body["worker_id"].get<std::string>(),
      status,
      request.body.value("message", "job completed"),
      artifacts,
      request.body.value("result", nlohmann::json::object()));
  return JsonResponse(200, SuccessEnvelope("completed platform job", SerializeJobRecord(*job)));
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
  return JsonResponse(200, SuccessEnvelope("registered workgroup cluster", memory_.RegisterWorkgroupCluster(workgroup_id, cluster)));
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
  else
  {
    clusters = memory_.ListWorkgroupClusters(workgroup_id);
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
          SuccessEnvelope("loaded mirror freshness", SerializeMirrorCursorRecord(*mirror)));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "mirror freshness unavailable", error.what(), "PostgresError", "mirrorStatus");
    }
  }
  const std::optional<MirrorCursorRecord> mirror = memory_.GetMirrorState(mirror_id);
  if (!mirror.has_value())
  {
    return FailureResponse(
        404,
        "mirror freshness unavailable",
        "mirror cursor not found",
        "MirrorError",
        "mirrorStatus");
  }
  nlohmann::json payload = SerializeMirrorCursorRecord(*mirror);
  return JsonResponse(200, SuccessEnvelope("loaded mirror freshness", payload));
}

bool PlatformRouter::RegistryWriteAuthorized(
    const HttpRequest &request,
    std::string_view scope,
    const std::string &package_id) const
{
  if (request.identity.has_value())
  {
    const MtlsIdentity &identity = *request.identity;
    if (identity.role == "operator")
    {
      return true;
    }
    if (scope == "package:publish" && identity.role == "registry-writer")
    {
      return true;
    }
    if ((scope == "package:publish" || scope == "package:yank" || scope == "package:owner") && !package_id.empty())
    {
      if (postgres_ != nullptr && postgres_->HasRegistryPackageOwner(package_id, identity.node_id))
      {
        return true;
      }
      if (memory_.HasRegistryPackageOwner(package_id, identity.node_id))
      {
        return true;
      }
    }
  }

  const std::optional<std::string> clear_token = RegistryTokenFromHeaders(request.headers);
  if (!clear_token.has_value())
  {
    return false;
  }
  const std::optional<RegistryPublishTokenRecord> token = postgres_ != nullptr
      ? postgres_->FindRegistryPublishTokenByHash(Sha256Bytes(*clear_token))
      : memory_.FindRegistryPublishTokenByHash(Sha256Bytes(*clear_token));
  if (!token.has_value())
  {
    return false;
  }
  const std::string now = UtcTimestampNow();
  if (!token->revoked_at.empty() || (!token->expires_at.empty() && token->expires_at < now))
  {
    return false;
  }
  return TokenHasScope(*token, scope) && TokenMatchesPackage(*token, package_id);
}

void PlatformRouter::RecordRegistryAudit(
    const HttpRequest &request,
    const std::string &operation,
    const nlohmann::json &target,
    const std::string &result)
{
  RegistryAuditEventRecord record{
      .event_id = postgres_ != nullptr ? postgres_->NextRegistryAuditEventId() : memory_.NextRegistryAuditEventId(),
      .actor_id = RegistryActorId(request),
      .operation = operation,
      .target = target,
      .request_id = request.headers.contains("x-request-id") ? request.headers.at("x-request-id") : "",
      .result = result,
      .created_at = UtcTimestampNow(),
  };
  if (postgres_ != nullptr)
  {
    postgres_->RecordRegistryAuditEvent(record);
  }
  memory_.RecordRegistryAuditEvent(record);
}

nlohmann::json PlatformRouter::CreateRegistryPublication(
    const std::string &change_kind,
    const std::string &change_ref,
    const std::string &generated_at)
{
  const fs::path registry_root(config_.registry.root);
  fs::create_directories(registry_root / "_publications");
  fs::create_directories(registry_root / "_distributions" / "default");

  const size_t disk_sequence = NextPublicationSequenceOnDisk(registry_root);
  const int memory_sequence = memory_.NextRepositoryVersionSequence("default");
  const int persistent_sequence = postgres_ != nullptr ? postgres_->NextRepositoryVersionSequence("default") : memory_sequence;
  const int sequence = static_cast<int>(std::max(disk_sequence, static_cast<size_t>(memory_sequence)));
  const int publication_sequence = std::max(sequence, persistent_sequence);
  const std::string repository_version_id = "rv-" + PaddedNumber(static_cast<size_t>(publication_sequence), 6);
  const std::string publication_id = "pub-" + PaddedNumber(static_cast<size_t>(publication_sequence), 6);

  const fs::path temp_root = registry_root / "_tmp" / "publications" / publication_id;
  const fs::path final_root = registry_root / "_publications" / publication_id;
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(temp_root.parent_path());
  CopyRegistryReadPlaneTo(registry_root, temp_root);
  WriteTextFile(
      temp_root / "config.json",
      JsonText(RegistryConfigPayload(config_, generated_at, publication_id, repository_version_id)));

  const size_t tree_size = LeafSequencePaths(temp_root).size();
  const size_t artifact_count = CountRegularFiles(temp_root / "artifacts");
  nlohmann::json publication = {
      {"publication_id", publication_id},
      {"repository_id", "default"},
      {"repository_version_id", repository_version_id},
      {"layout_version", 2},
      {"generated_at", generated_at},
      {"tree_size", static_cast<int64_t>(tree_size)},
      {"index_digest", DirectoryDigest(temp_root / "index")},
      {"trust_digest", DirectoryDigest(temp_root / "trust")},
      {"artifact_count", static_cast<int64_t>(artifact_count)},
      {"verified", true},
  };
  WriteTextFile(temp_root / "publication.json", JsonText(publication));
  if (!fs::exists(temp_root / "config.json") || !fs::exists(temp_root / "trust" / "root.json") ||
      !fs::exists(temp_root / "publication.json"))
  {
    throw std::runtime_error("publication verification failed before distribution update");
  }

  if (fs::exists(final_root, ec))
  {
    throw std::runtime_error("publication already exists: " + publication_id);
  }
  fs::rename(temp_root, final_root, ec);
  if (ec)
  {
    throw std::runtime_error("failed to publish immutable publication: " + ec.message());
  }
  fs::copy_file(final_root / "config.json", registry_root / "config.json", fs::copy_options::overwrite_existing, ec);
  if (ec)
  {
    throw std::runtime_error("failed to activate publication config: " + ec.message());
  }

  const std::optional<nlohmann::json> previous = CurrentDistributionPointer(registry_root, "default");
  const std::string previous_publication_id =
      previous.has_value() ? previous->value("publication_id", std::string()) : std::string();
  nlohmann::json current = {
      {"distribution_id", "default"},
      {"publication_id", publication_id},
      {"repository_version_id", repository_version_id},
      {"updated_at", generated_at},
  };
  if (!previous_publication_id.empty())
  {
    current["previous_publication_id"] = previous_publication_id;
  }
  WriteTextFile(registry_root / "_distributions" / "default" / "current.json", JsonText(current));

  memory_.UpsertRegistryRepository({
      .repository_id = "default",
      .name = "default",
      .tenant_id = "platform",
      .policy = {{"immutable_artifacts", true}, {"append_only_versions", true}},
      .created_at = generated_at,
  });
  memory_.RecordRegistryRepositoryVersion({
      .repository_version_id = repository_version_id,
      .repository_id = "default",
      .sequence = publication_sequence,
      .change_kind = change_kind,
      .change_ref = change_ref,
      .created_at = generated_at,
  });
  memory_.RecordRegistryPublication({
      .publication_id = publication_id,
      .repository_version_id = repository_version_id,
      .layout_version = 2,
      .root_path = (registry_root / "_publications" / publication_id).string(),
      .manifest_sha256 = spio::Sha256File(final_root / "publication.json"),
      .tree_size = static_cast<int>(tree_size),
      .created_at = generated_at,
      .verified = true,
  });
  memory_.UpsertRegistryDistribution({
      .distribution_id = "default",
      .repository_id = "default",
      .name = "default",
      .base_url = RegistryReadRootUrl(config_),
      .current_publication_id = publication_id,
      .previous_publication_id = previous_publication_id,
      .updated_at = generated_at,
  });
  if (postgres_ != nullptr)
  {
    postgres_->UpsertRegistryRepository({
        .repository_id = "default",
        .name = "default",
        .tenant_id = "platform",
        .policy = {{"immutable_artifacts", true}, {"append_only_versions", true}},
        .created_at = generated_at,
    });
    postgres_->RecordRegistryRepositoryVersion({
        .repository_version_id = repository_version_id,
        .repository_id = "default",
        .sequence = publication_sequence,
        .change_kind = change_kind,
        .change_ref = change_ref,
        .created_at = generated_at,
    });
    postgres_->RecordRegistryPublication({
        .publication_id = publication_id,
        .repository_version_id = repository_version_id,
        .layout_version = 2,
        .root_path = (registry_root / "_publications" / publication_id).string(),
        .manifest_sha256 = spio::Sha256File(final_root / "publication.json"),
        .tree_size = static_cast<int>(tree_size),
        .created_at = generated_at,
        .verified = true,
    });
    postgres_->UpsertRegistryDistribution({
        .distribution_id = "default",
        .repository_id = "default",
        .name = "default",
        .base_url = RegistryReadRootUrl(config_),
        .current_publication_id = publication_id,
        .previous_publication_id = previous_publication_id,
        .updated_at = generated_at,
    });
  }
  RecordMirrorState(
      "fresh",
      "checkpoint-" + PaddedNumber(tree_size, 4),
      publication_id,
      repository_version_id,
      generated_at,
      static_cast<int>(tree_size));

  publication["manifest_sha256"] = spio::Sha256File(final_root / "publication.json");
  publication["root_path"] = (registry_root / "_publications" / publication_id).generic_string();
  publication["distribution"] = current;
  return publication;
}

HttpResponse PlatformRouter::HandleRegistryStatus() const
{
  if (UsesS3ObjectStore(config_))
  {
    try
    {
      const bool config_present = ObjectExists(config_.object_store, "config.json");
      const bool root_metadata_present = ObjectExists(config_.object_store, "trust/root.json");
      nlohmann::json payload = {
          {"registry_root", "<redacted>"},
          {"key_dir", "<redacted>"},
          {"registry_name", config_.registry.registry_name},
          {"object_store_provider", config_.object_store.provider},
          {"root_initialized", config_present && root_metadata_present},
          {"config_present", config_present},
          {"root_metadata_present", root_metadata_present},
          {"publish_endpoint", "/api/spio-registry-control/v1/publish"},
          {"verify_endpoint", "/api/spio-registry-control/v1/verify"},
          {"descriptor_endpoint", "/api/spio-registry-control/v1/descriptor"},
      };
      return JsonResponse(200, SuccessEnvelope("registry control plane is ready", payload));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "registry status failed", error.what(), "RegistryStatusError", "registryStatus");
    }
  }

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
      {"descriptor_endpoint", "/api/spio-registry-control/v1/descriptor"},
  };
  return JsonResponse(200, SuccessEnvelope("registry control plane is ready", payload));
}

HttpResponse PlatformRouter::HandleRegistryDescriptor() const
{
  if (UsesS3ObjectStore(config_))
  {
    try
    {
      const std::optional<std::string> root_metadata = GetObjectText(config_.object_store, "trust/root.json");
      if (!root_metadata.has_value())
      {
        return FailureResponse(
            422,
            "registry descriptor failed",
            "registry root metadata is not initialized",
            "RegistryDescriptorError",
            "registryDescriptor");
      }
      nlohmann::json payload = {
          {"schema_version", 1},
          {"registry_name", config_.registry.registry_name},
          {"registry_root", RegistryReadRootUrl(config_)},
          {"control_plane_base_url", RegistryControlPlaneBaseUrl(config_)},
          {"root_sha256", Sha256Bytes(*root_metadata)},
          {"issued_at", UtcTimestampNow()},
          {"expires", UtcTimestampPlusDays(31)},
          {"descriptor_signature", "platform-control-plane-mtls"},
      };
      return JsonResponse(200, SuccessEnvelope("published registry trust descriptor", payload));
    }
    catch (const std::exception &error)
    {
      return FailureResponse(503, "registry descriptor failed", error.what(), "RegistryDescriptorError", "registryDescriptor");
    }
  }

  const fs::path registry_root(config_.registry.root);
  const fs::path root_metadata = registry_root / "trust" / "root.json";
  std::error_code ec;
  if (!fs::exists(root_metadata, ec))
  {
    return FailureResponse(
        422,
        "registry descriptor failed",
        "registry root metadata is not initialized",
        "RegistryDescriptorError",
        "registryDescriptor");
  }
  nlohmann::json payload = {
      {"schema_version", 1},
      {"registry_name", config_.registry.registry_name},
      {"registry_root", RegistryReadRootUrl(config_)},
      {"control_plane_base_url", RegistryControlPlaneBaseUrl(config_)},
      {"root_sha256", spio::Sha256File(root_metadata)},
      {"issued_at", "2026-05-02T00:00:00Z"},
      {"expires", "2026-06-02T00:00:00Z"},
      {"descriptor_signature", "platform-control-plane-mtls"},
  };
  return JsonResponse(200, SuccessEnvelope("published registry trust descriptor", payload));
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
  if (!RegistryWriteAuthorized(request, "package:publish", RegistryPackageId(draft.package)))
  {
    RecordRegistryAudit(
        request,
        "publishRelease",
        {{"package_id", draft.package}, {"version", draft.version}},
        "denied");
    return FailureResponse(403, "registry publish denied", "token or identity lacks package:publish", "AuthError", "publishRelease", 2);
  }

  try
  {
    const fs::path registry_root(config_.registry.root);
    const std::string release_key = RegistryReleaseKey(draft.package, draft.version);
    if (UsesS3ObjectStore(config_))
    {
      const bool remote_initialized = ObjectExists(config_.object_store, "config.json") ||
                                      ObjectExists(config_.object_store, "trust/root.json");
      if (remote_initialized)
      {
        SyncS3RegistryStateToLocal(config_);
      }
      else
      {
        RemoveLocalRegistryMetadataCache(config_);
      }
    }

    const bool release_exists_in_postgres =
        postgres_ != nullptr && postgres_->GetRegistryPackageRelease(RegistryPackageId(draft.package), draft.version).has_value();
    if (memory_.HasPublishedRelease(release_key) || release_exists_in_postgres ||
        ReleaseExistsOnDisk(registry_root, draft.package, draft.version))
    {
      RecordRegistryAudit(
          request,
          "publishRelease",
          {{"package_id", draft.package}, {"version", draft.version}},
          "duplicate");
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
    const std::map<std::string, RegistryRoleKey> role_keys = LoadOrCreateRegistryRoleKeys(fs::path(config_.registry.key_dir));

    const std::string archive_sha256 = spio::Sha256File(draft.archive_path);
    const uintmax_t archive_size = fs::file_size(draft.archive_path);
    const std::string artifact_path =
        "artifacts/source/sha256/" + archive_sha256.substr(0, 2) + "/" + archive_sha256.substr(2, 2) + "/" +
        archive_sha256 + ".spio.src.tar";
    const std::string published_at = UtcTimestampNow();
    const nlohmann::json release_record =
        BuildReleaseRecord(draft, published_at, archive_sha256, archive_size, artifact_path);
    const LocalAppendResult append_result =
        AppendRegistryReleaseToLocal(config_, draft, release_record, artifact_path);
    const MetadataVersions metadata_versions = RefreshSignedRegistryMetadata(config_, role_keys, published_at);
    const nlohmann::json publication =
        CreateRegistryPublication("publish", RegistryReleaseKey(draft.package, draft.version), published_at);
    if (UsesS3ObjectStore(config_))
    {
      UploadRegistryTreeToS3(config_);
    }

    nlohmann::json payload = {
        {"registry_root", registry_root.string()},
        {"registry_read_root", RegistryReadRootUrl(config_)},
        {"object_store_provider", config_.object_store.provider},
        {"created_root", created_root},
        {"package", draft.package},
        {"version", draft.version},
        {"publisher_id", draft.publisher_id},
        {"published_at", published_at},
        {"archive_path", draft.archive_path.string()},
        {"archive_sha256", archive_sha256},
        {"archive_size_bytes", static_cast<int64_t>(archive_size)},
        {"artifact_path", artifact_path},
        {"index_path", append_result.index_path},
        {"log_leaf_path", append_result.log_leaf_path},
        {"sequence", static_cast<int64_t>(append_result.sequence)},
        {"dependencies", draft.dependencies},
        {"dev_dependencies", draft.dev_dependencies},
        {"checkpoint_version", metadata_versions.checkpoint_version},
        {"snapshot_version", metadata_versions.snapshot_version},
        {"timestamp_version", metadata_versions.timestamp_version},
        {"namespaces", static_cast<int64_t>(metadata_versions.namespaces)},
        {"repository_id", "default"},
        {"repository_version_id", publication.at("repository_version_id").get<std::string>()},
        {"publication_id", publication.at("publication_id").get<std::string>()},
        {"distribution_id", "default"},
    };
    memory_.RecordPublishedRelease(release_key, payload);
    const size_t slash = draft.package.find('/');
    RegistryPackageRecord package_record{
        .package_id = RegistryPackageId(draft.package),
        .package_namespace = draft.package.substr(0, slash),
        .name = draft.package.substr(slash + 1),
        .created_at = published_at,
        .created_by = draft.publisher_id,
        .visibility = "public",
    };
    RegistryPackageReleaseRecord release_record_state{
        .package_id = RegistryPackageId(draft.package),
        .version = draft.version,
        .edition = "2026",
        .manifest_sha256 = release_record.at("manifest_digest").get<std::string>(),
        .source_artifact_sha256 = archive_sha256,
        .dependencies = release_record.at("dependencies"),
        .publisher_id = draft.publisher_id,
        .published_at = published_at,
        .yanked = false,
        .yanked_reason = "",
    };
    RegistryPackageOwnerRecord owner_record{
        .package_id = RegistryPackageId(draft.package),
        .owner_id = draft.publisher_id,
        .owner_kind = "user",
        .role = "owner",
        .added_by = RegistryActorId(request),
        .added_at = published_at,
    };
    if (postgres_ != nullptr)
    {
      postgres_->UpsertRegistryPackage(package_record);
      postgres_->UpsertRegistryPackageRelease(release_record_state);
      postgres_->AddRegistryPackageOwner(owner_record);
    }
    memory_.UpsertRegistryPackage(package_record);
    memory_.UpsertRegistryPackageRelease(release_record_state);
    memory_.AddRegistryPackageOwner(owner_record);
    RecordRegistryAudit(
        request,
        "publishRelease",
        {{"package_id", draft.package}, {"version", draft.version}, {"publication_id", publication.at("publication_id")}},
        "success");
    return JsonResponse(200, SuccessEnvelope("published registry v2 release", payload));
  }
  catch (const std::exception &error)
  {
    RecordRegistryAudit(
        request,
        "publishRelease",
        {{"package_id", draft.package}, {"version", draft.version}},
        "failed");
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
    if (UsesS3ObjectStore(config_))
    {
      if (!ObjectExists(config_.object_store, "config.json") || !ObjectExists(config_.object_store, "trust/root.json"))
      {
        return FailureResponse(
            422,
            "registry verification failed",
            "registry root is not initialized",
            "VerifyError",
            "verifyRegistry");
      }
      SyncS3RegistryStateToLocal(config_);
    }
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
        {"registry_read_root", RegistryReadRootUrl(config_)},
        {"object_store_provider", config_.object_store.provider},
        {"namespaces", static_cast<int64_t>(namespaces)},
        {"index_files", static_cast<int64_t>(index_files)},
        {"releases", static_cast<int64_t>(releases)},
        {"tree_size", static_cast<int64_t>(tree_size)},
    };
    const std::optional<nlohmann::json> current = CurrentDistributionPointer(registry_root, "default");
    RecordMirrorState(
        "fresh",
        "checkpoint-" + PaddedNumber(tree_size, 4),
        current.has_value() ? current->value("publication_id", "") : "",
        current.has_value() ? current->value("repository_version_id", "") : "",
        UtcTimestampNow(),
        static_cast<int>(tree_size));
    return JsonResponse(200, SuccessEnvelope("verified registry v2 root", payload));
  }
  catch (const std::exception &error)
  {
    return FailureResponse(422, "registry verification failed", error.what(), "VerifyError", "verifyRegistry");
  }
}

HttpResponse PlatformRouter::HandleGetPackage(const RouteMatch &match) const
{
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value())
  {
    return FailureResponse(400, "package lookup rejected", *error, "ValidationError", "getPackage", 2);
  }
  try
  {
    if (postgres_ != nullptr)
    {
      const std::optional<RegistryPackageRecord> record = postgres_->GetRegistryPackage(RegistryPackageId(package));
      if (record.has_value())
      {
        nlohmann::json payload = SerializeRegistryPackageRecord(*record);
        payload["releases"] = postgres_->ListRegistryPackageReleases(RegistryPackageId(package));
        payload["release_count"] = payload.at("releases").size();
        return JsonResponse(200, SuccessEnvelope("loaded package", payload));
      }
    }
    return JsonResponse(
        200,
        SuccessEnvelope(
            "loaded package",
            PackagePayloadFromIndex(fs::path(config_.registry.root), package)));
  }
  catch (const std::exception &error)
  {
    return FailureResponse(404, "package lookup failed", error.what(), "NotFound", "getPackage");
  }
}

HttpResponse PlatformRouter::HandleListPackageReleases(const RouteMatch &match) const
{
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value())
  {
    return FailureResponse(400, "package release lookup rejected", *error, "ValidationError", "listPackageReleases", 2);
  }
  const fs::path root(config_.registry.root);
  const std::vector<nlohmann::json> releases = ReadPackageIndexRecords(root, package);
  if (releases.empty())
  {
    if (postgres_ != nullptr)
    {
      nlohmann::json persisted_releases = postgres_->ListRegistryPackageReleases(RegistryPackageId(package));
      if (!persisted_releases.empty())
      {
        return JsonResponse(
            200,
            SuccessEnvelope(
                "loaded package releases",
                {{"package_id", package}, {"releases", persisted_releases}}));
      }
    }
    return FailureResponse(404, "package release lookup failed", "package is not found", "NotFound", "listPackageReleases");
  }
  return JsonResponse(
      200,
      SuccessEnvelope(
          "loaded package releases",
          {{"package_id", package}, {"releases", releases}}));
}

HttpResponse PlatformRouter::HandleGetPackageRelease(const RouteMatch &match) const
{
  const std::string package = RegistryPackageFromRoute(match);
  const std::string version = match.parameters.at("version");
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value())
  {
    return FailureResponse(400, "package release lookup rejected", *error, "ValidationError", "getPackageRelease", 2);
  }
  const std::optional<nlohmann::json> release =
      PackageReleasePayloadFromIndex(fs::path(config_.registry.root), package, version);
  if (!release.has_value())
  {
    if (postgres_ != nullptr)
    {
      const std::optional<RegistryPackageReleaseRecord> persisted_release =
          postgres_->GetRegistryPackageRelease(RegistryPackageId(package), version);
      if (persisted_release.has_value())
      {
        return JsonResponse(
            200,
            SuccessEnvelope("loaded package release", SerializeRegistryPackageReleaseRecord(*persisted_release)));
      }
    }
    return FailureResponse(404, "package release lookup failed", "package release is not found", "NotFound", "getPackageRelease");
  }
  return JsonResponse(200, SuccessEnvelope("loaded package release", *release));
}

HttpResponse PlatformRouter::HandleSetPackageReleaseYanked(
    const RouteMatch &match,
    const HttpRequest &request,
    const bool yanked)
{
  const std::string package = RegistryPackageFromRoute(match);
  const std::string version = match.parameters.at("version");
  const std::string operation = yanked ? "yankPackageRelease" : "unyankPackageRelease";
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value())
  {
    return FailureResponse(400, "package release mutation rejected", *error, "ValidationError", operation, 2);
  }
  if (!request.body.is_object())
  {
    return FailureResponse(400, "package release mutation rejected", "request body must be an object", "ValidationError", operation, 2);
  }
  if (request.body.contains("reason") && !request.body["reason"].is_string())
  {
    return FailureResponse(400, "package release mutation rejected", "reason must be a string when present", "ValidationError", operation, 2);
  }
  if (!RegistryWriteAuthorized(request, yanked ? "package:yank" : "package:yank", RegistryPackageId(package)))
  {
    RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "denied");
    return FailureResponse(403, "package release mutation denied", "token or identity lacks package:yank", "AuthError", operation, 2);
  }

  try
  {
    const fs::path root(config_.registry.root);
    std::vector<nlohmann::json> records = ReadPackageIndexRecords(root, package);
    bool found = false;
    std::string artifact_path;
    for (nlohmann::json &record : records)
    {
      if (record.value("version", "") == version)
      {
        found = true;
        record["yanked"] = yanked;
        record["yanked_reason"] = yanked ? request.body.value("reason", std::string()) : "";
        if (yanked)
        {
          record["yanked_at"] = UtcTimestampNow();
        }
        else
        {
          record.erase("yanked_at");
        }
        artifact_path = record.at("source_artifact").at("path").get<std::string>();
      }
    }
    if (!found)
    {
      RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "not_found");
      return FailureResponse(404, "package release mutation failed", "package release is not found", "NotFound", operation);
    }
    WritePackageIndexRecords(root, package, records);
    const std::map<std::string, RegistryRoleKey> role_keys = LoadOrCreateRegistryRoleKeys(fs::path(config_.registry.key_dir));
    const std::string changed_at = UtcTimestampNow();
    const MetadataVersions metadata_versions = RefreshSignedRegistryMetadata(config_, role_keys, changed_at);
    const nlohmann::json publication =
        CreateRegistryPublication(yanked ? "yank" : "unyank", RegistryReleaseKey(package, version), changed_at);
    const std::string yank_reason = yanked ? request.body.value("reason", std::string()) : std::string();
    if (postgres_ != nullptr)
    {
      postgres_->SetRegistryPackageReleaseYanked(RegistryPackageId(package), version, yanked, yank_reason);
    }
    memory_.SetRegistryPackageReleaseYanked(RegistryPackageId(package), version, yanked, yank_reason);
    RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "success");
    if (UsesS3ObjectStore(config_))
    {
      UploadRegistryTreeToS3(config_);
    }
    return JsonResponse(
        200,
        SuccessEnvelope(
            yanked ? "yanked package release" : "unyanked package release",
            {
                {"package_id", package},
                {"version", version},
                {"yanked", yanked},
                {"artifact_path", artifact_path},
                {"artifact_preserved", fs::exists(root / artifact_path)},
                {"checkpoint_version", metadata_versions.checkpoint_version},
                {"snapshot_version", metadata_versions.snapshot_version},
                {"timestamp_version", metadata_versions.timestamp_version},
                {"repository_version_id", publication.at("repository_version_id")},
                {"publication_id", publication.at("publication_id")},
                {"distribution_id", "default"},
            }));
  }
  catch (const std::exception &error)
  {
    RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "failed");
    return FailureResponse(422, "package release mutation failed", error.what(), "PublishError", operation);
  }
}

HttpResponse PlatformRouter::HandleListPackageOwners(const RouteMatch &match) const
{
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value())
  {
    return FailureResponse(400, "package owner lookup rejected", *error, "ValidationError", "listPackageOwners", 2);
  }
  if (ReadPackageIndexRecords(fs::path(config_.registry.root), package).empty())
  {
    return FailureResponse(404, "package owner lookup failed", "package is not found", "NotFound", "listPackageOwners");
  }
  nlohmann::json owners = postgres_ != nullptr
      ? postgres_->ListRegistryPackageOwners(RegistryPackageId(package))
      : memory_.ListRegistryPackageOwners(RegistryPackageId(package));
  if (owners.empty())
  {
    const std::vector<nlohmann::json> releases = ReadPackageIndexRecords(fs::path(config_.registry.root), package);
    if (!releases.empty())
    {
      owners.push_back({
          {"package_id", package},
          {"owner_id", releases.front().value("publisher_id", "")},
          {"owner_kind", "user"},
          {"role", "owner"},
          {"added_by", "registry-index"},
          {"added_at", releases.front().value("published_at", "")},
      });
    }
  }
  return JsonResponse(200, SuccessEnvelope("loaded package owners", {{"package_id", package}, {"owners", owners}}));
}

HttpResponse PlatformRouter::HandleAddPackageOwner(const RouteMatch &match, const HttpRequest &request)
{
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value())
  {
    return FailureResponse(400, "package owner mutation rejected", *error, "ValidationError", "addPackageOwner", 2);
  }
  if (!request.body.is_object() || !HasNonEmptyString(request.body, "owner_id"))
  {
    return FailureResponse(400, "package owner mutation rejected", "owner_id is required", "ValidationError", "addPackageOwner", 2);
  }
  if (!RegistryWriteAuthorized(request, "package:owner", RegistryPackageId(package)))
  {
    RecordRegistryAudit(request, "addPackageOwner", {{"package_id", package}, {"owner_id", request.body.value("owner_id", "")}}, "denied");
    return FailureResponse(403, "package owner mutation denied", "token or identity lacks package:owner", "AuthError", "addPackageOwner", 2);
  }
  if (ReadPackageIndexRecords(fs::path(config_.registry.root), package).empty())
  {
    RecordRegistryAudit(request, "addPackageOwner", {{"package_id", package}, {"owner_id", request.body.value("owner_id", "")}}, "not_found");
    return FailureResponse(404, "package owner mutation failed", "package is not found", "NotFound", "addPackageOwner");
  }
  RegistryPackageOwnerRecord owner{
      .package_id = package,
      .owner_id = request.body.at("owner_id").get<std::string>(),
      .owner_kind = request.body.value("owner_kind", "user"),
      .role = request.body.value("role", "owner"),
      .added_by = RegistryActorId(request),
      .added_at = UtcTimestampNow(),
  };
  if (postgres_ != nullptr)
  {
    postgres_->AddRegistryPackageOwner(owner);
  }
  memory_.AddRegistryPackageOwner(owner);
  RecordRegistryAudit(request, "addPackageOwner", {{"package_id", package}, {"owner_id", owner.owner_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("added package owner", SerializeRegistryPackageOwnerRecord(owner)));
}

HttpResponse PlatformRouter::HandleRemovePackageOwner(const RouteMatch &match, const HttpRequest &request)
{
  const std::string package = RegistryPackageFromRoute(match);
  const std::string owner_id = match.parameters.at("owner_id");
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value())
  {
    return FailureResponse(400, "package owner mutation rejected", *error, "ValidationError", "removePackageOwner", 2);
  }
  if (!RegistryWriteAuthorized(request, "package:owner", RegistryPackageId(package)))
  {
    RecordRegistryAudit(request, "removePackageOwner", {{"package_id", package}, {"owner_id", owner_id}}, "denied");
    return FailureResponse(403, "package owner mutation denied", "token or identity lacks package:owner", "AuthError", "removePackageOwner", 2);
  }
  const bool removed = postgres_ != nullptr
      ? postgres_->RemoveRegistryPackageOwner(RegistryPackageId(package), owner_id)
      : memory_.RemoveRegistryPackageOwner(RegistryPackageId(package), owner_id);
  if (removed)
  {
    memory_.RemoveRegistryPackageOwner(RegistryPackageId(package), owner_id);
  }
  if (!removed)
  {
    RecordRegistryAudit(request, "removePackageOwner", {{"package_id", package}, {"owner_id", owner_id}}, "not_found");
    return FailureResponse(404, "package owner mutation failed", "package owner is not found", "NotFound", "removePackageOwner");
  }
  RecordRegistryAudit(request, "removePackageOwner", {{"package_id", package}, {"owner_id", owner_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("removed package owner", {{"package_id", package}, {"owner_id", owner_id}}));
}

HttpResponse PlatformRouter::HandleCreatePublishToken(const HttpRequest &request)
{
  if (!request.body.is_object())
  {
    return FailureResponse(400, "publish token creation rejected", "request body must be an object", "ValidationError", "createPublishToken", 2);
  }
  if (const std::optional<std::string> error = ValidateStringArray(request.body, "scopes"); error.has_value())
  {
    return FailureResponse(400, "publish token creation rejected", *error, "ValidationError", "createPublishToken", 2);
  }
  if (const std::optional<std::string> error = ValidateStringArray(request.body, "package_patterns"); error.has_value())
  {
    return FailureResponse(400, "publish token creation rejected", *error, "ValidationError", "createPublishToken", 2);
  }
  const std::string actor = RegistryActorId(request);
  const std::string owner_id = request.body.value("owner_id", actor);
  if (!request.identity.has_value() || (request.identity->role != "operator" && owner_id != actor))
  {
    RecordRegistryAudit(request, "createPublishToken", {{"owner_id", owner_id}}, "denied");
    return FailureResponse(403, "publish token creation denied", "token owner must match caller", "AuthError", "createPublishToken", 2);
  }
  const std::string token_id = postgres_ != nullptr ? postgres_->NextRegistryTokenId() : memory_.NextRegistryTokenId();
  const std::string clear_token = "styio_pat_" + token_id + "_" + SecureRandomHex(24);
  RegistryPublishTokenRecord token{
      .token_id = token_id,
      .token_hash = Sha256Bytes(clear_token),
      .owner_id = owner_id,
      .scopes = JsonStringArray(request.body, "scopes", {"package:publish"}),
      .package_patterns = JsonStringArray(request.body, "package_patterns", {"*"}),
      .expires_at = request.body.value("expires_at", ""),
      .revoked_at = "",
      .created_at = UtcTimestampNow(),
  };
  if (postgres_ != nullptr)
  {
    postgres_->UpsertRegistryPublishToken(token);
  }
  memory_.UpsertRegistryPublishToken(token);
  nlohmann::json payload = SerializeRegistryPublishTokenRecord(token);
  payload["token"] = clear_token;
  RecordRegistryAudit(request, "createPublishToken", {{"token_id", token_id}, {"owner_id", owner_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("created publish token", payload));
}

HttpResponse PlatformRouter::HandleListPublishTokens(const HttpRequest &request) const
{
  const std::string owner_filter =
      request.identity.has_value() && request.identity->role == "operator" ? std::string() : RegistryActorId(request);
  return JsonResponse(
      200,
      SuccessEnvelope(
          "loaded publish tokens",
          {{"tokens", postgres_ != nullptr ? postgres_->ListRegistryPublishTokens(owner_filter)
                                           : memory_.ListRegistryPublishTokens(owner_filter)}}));
}

HttpResponse PlatformRouter::HandleRevokePublishToken(const RouteMatch &match, const HttpRequest &request)
{
  const std::string token_id = match.parameters.at("token_id");
  const std::optional<RegistryPublishTokenRecord> token = postgres_ != nullptr
      ? postgres_->GetRegistryPublishToken(token_id)
      : memory_.GetRegistryPublishToken(token_id);
  if (!token.has_value())
  {
    RecordRegistryAudit(request, "revokePublishToken", {{"token_id", token_id}}, "not_found");
    return FailureResponse(404, "publish token revocation failed", "token is not found", "NotFound", "revokePublishToken");
  }
  const std::string actor = RegistryActorId(request);
  if (!request.identity.has_value() || (request.identity->role != "operator" && token->owner_id != actor))
  {
    RecordRegistryAudit(request, "revokePublishToken", {{"token_id", token_id}}, "denied");
    return FailureResponse(403, "publish token revocation denied", "token owner must match caller", "AuthError", "revokePublishToken", 2);
  }
  const std::string revoked_at = UtcTimestampNow();
  if (postgres_ != nullptr)
  {
    postgres_->RevokeRegistryPublishToken(token_id, revoked_at);
  }
  memory_.RevokeRegistryPublishToken(token_id, revoked_at);
  RecordRegistryAudit(request, "revokePublishToken", {{"token_id", token_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("revoked publish token", {{"token_id", token_id}, {"revoked", true}}));
}

HttpResponse PlatformRouter::HandleListRepositories() const
{
  nlohmann::json repositories =
      postgres_ != nullptr ? postgres_->ListRegistryRepositories() : memory_.ListRegistryRepositories();
  if (repositories.empty())
  {
    repositories.push_back({
        {"repository_id", "default"},
        {"name", "default"},
        {"tenant_id", "platform"},
        {"policy", {{"immutable_artifacts", true}, {"append_only_versions", true}}},
        {"created_at", ""},
      });
  }
  return JsonResponse(200, SuccessEnvelope("loaded repositories", {{"repositories", repositories}}));
}

HttpResponse PlatformRouter::HandleListRepositoryVersions(const RouteMatch &match) const
{
  const std::string repository_id = match.parameters.at("repository_id");
  return JsonResponse(
      200,
      SuccessEnvelope(
          "loaded repository versions",
          {{"repository_id", repository_id},
           {"versions", postgres_ != nullptr ? postgres_->ListRegistryRepositoryVersions(repository_id)
                                             : memory_.ListRegistryRepositoryVersions(repository_id)}}));
}

HttpResponse PlatformRouter::HandleGetPublication(const RouteMatch &match) const
{
  const std::string publication_id = match.parameters.at("publication_id");
  const fs::path publication_path = fs::path(config_.registry.root) / "_publications" / publication_id / "publication.json";
  if (!fs::exists(publication_path))
  {
    return FailureResponse(404, "publication lookup failed", "publication is not found", "NotFound", "getPublication");
  }
  nlohmann::json payload = nlohmann::json::parse(ReadFileBytes(publication_path));
  const std::optional<RegistryPublicationRecord> record =
      postgres_ != nullptr ? postgres_->GetRegistryPublication(publication_id) : memory_.GetRegistryPublication(publication_id);
  if (record.has_value())
  {
    payload["control_plane_record"] = SerializeRegistryPublicationRecord(*record);
  }
  return JsonResponse(200, SuccessEnvelope("loaded publication", payload));
}

HttpResponse PlatformRouter::HandleVerifyPublication(const RouteMatch &match, const HttpRequest &request)
{
  if (!request.body.is_object() || !request.body.empty())
  {
    return FailureResponse(400, "publication verification rejected", "verify request must be an empty JSON object", "VerifyError", "verifyPublication", 2);
  }
  const std::string publication_id = match.parameters.at("publication_id");
  const fs::path publication_root = fs::path(config_.registry.root) / "_publications" / publication_id;
  const fs::path publication_path = publication_root / "publication.json";
  if (!fs::exists(publication_path))
  {
    return FailureResponse(404, "publication verification failed", "publication is not found", "NotFound", "verifyPublication");
  }
  try
  {
    nlohmann::json publication = nlohmann::json::parse(ReadFileBytes(publication_path));
    const bool index_ok = publication.value("index_digest", "") == DirectoryDigest(publication_root / "index");
    const bool trust_ok = publication.value("trust_digest", "") == DirectoryDigest(publication_root / "trust");
    if (!index_ok || !trust_ok)
    {
      return FailureResponse(422, "publication verification failed", "publication digest mismatch", "VerifyError", "verifyPublication");
    }
    publication["verified"] = true;
    return JsonResponse(200, SuccessEnvelope("verified publication", publication));
  }
  catch (const std::exception &error)
  {
    return FailureResponse(422, "publication verification failed", error.what(), "VerifyError", "verifyPublication");
  }
}

HttpResponse PlatformRouter::HandleListDistributions() const
{
  nlohmann::json distributions =
      postgres_ != nullptr ? postgres_->ListRegistryDistributions() : memory_.ListRegistryDistributions();
  if (distributions.empty())
  {
    const std::optional<nlohmann::json> current = CurrentDistributionPointer(fs::path(config_.registry.root), "default");
    distributions.push_back({
        {"distribution_id", "default"},
        {"repository_id", "default"},
        {"name", "default"},
        {"base_url", RegistryReadRootUrl(config_)},
        {"current_publication_id", current.has_value() ? current->value("publication_id", "") : ""},
        {"previous_publication_id", current.has_value() ? current->value("previous_publication_id", "") : ""},
        {"updated_at", current.has_value() ? current->value("updated_at", "") : ""},
    });
  }
  return JsonResponse(200, SuccessEnvelope("loaded distributions", {{"distributions", distributions}}));
}

HttpResponse PlatformRouter::HandlePromoteDistribution(const RouteMatch &match, const HttpRequest &request)
{
  const std::string distribution_id = match.parameters.at("distribution_id");
  if (!request.body.is_object() || !HasNonEmptyString(request.body, "publication_id"))
  {
    return FailureResponse(400, "distribution promotion rejected", "publication_id is required", "ValidationError", "promoteDistribution", 2);
  }
  if (!RegistryWriteAuthorized(request, "repository:promote", ""))
  {
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}}, "denied");
    return FailureResponse(403, "distribution promotion denied", "token or identity lacks repository:promote", "AuthError", "promoteDistribution", 2);
  }
  const std::string publication_id = request.body.at("publication_id").get<std::string>();
  const fs::path registry_root(config_.registry.root);
  const fs::path publication_root = registry_root / "_publications" / publication_id;
  const fs::path publication_path = publication_root / "publication.json";
  if (!fs::exists(publication_path))
  {
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}, {"publication_id", publication_id}}, "not_found");
    return FailureResponse(404, "distribution promotion failed", "publication is not found", "NotFound", "promoteDistribution");
  }
  try
  {
    const nlohmann::json publication = nlohmann::json::parse(ReadFileBytes(publication_path));
    const std::optional<nlohmann::json> previous = CurrentDistributionPointer(registry_root, distribution_id);
    if (distribution_id == "default")
    {
      MaterializePublicationToRoot(registry_root, publication_root);
    }
    const std::string updated_at = UtcTimestampNow();
    const std::string previous_publication_id =
        previous.has_value() ? previous->value("publication_id", std::string()) : std::string();
    nlohmann::json current = {
        {"distribution_id", distribution_id},
        {"publication_id", publication_id},
        {"repository_version_id", publication.at("repository_version_id").get<std::string>()},
        {"updated_at", updated_at},
    };
    if (!previous_publication_id.empty())
    {
      current["previous_publication_id"] = previous_publication_id;
    }
    WriteTextFile(registry_root / "_distributions" / distribution_id / "current.json", JsonText(current));
    RegistryDistributionRecord distribution_record{
        .distribution_id = distribution_id,
        .repository_id = publication.value("repository_id", "default"),
        .name = distribution_id,
        .base_url = RegistryReadRootUrl(config_),
        .current_publication_id = publication_id,
        .previous_publication_id = previous_publication_id,
        .updated_at = updated_at,
    };
    if (postgres_ != nullptr)
    {
      postgres_->UpsertRegistryDistribution(distribution_record);
    }
    memory_.UpsertRegistryDistribution(distribution_record);
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}, {"publication_id", publication_id}}, "success");
    return JsonResponse(200, SuccessEnvelope("promoted distribution", current));
  }
  catch (const std::exception &error)
  {
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}, {"publication_id", publication_id}}, "failed");
    return FailureResponse(422, "distribution promotion failed", error.what(), "PublishError", "promoteDistribution");
  }
}

HttpResponse PlatformRouter::HandleRollbackDistribution(const RouteMatch &match, const HttpRequest &request)
{
  const std::string distribution_id = match.parameters.at("distribution_id");
  if (!RegistryWriteAuthorized(request, "repository:promote", ""))
  {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}}, "denied");
    return FailureResponse(403, "distribution rollback denied", "token or identity lacks repository:promote", "AuthError", "rollbackDistribution", 2);
  }
  const fs::path registry_root(config_.registry.root);
  const std::optional<nlohmann::json> current_pointer = CurrentDistributionPointer(registry_root, distribution_id);
  if (!current_pointer.has_value() || current_pointer->value("previous_publication_id", std::string()).empty())
  {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}}, "no_previous_publication");
    return FailureResponse(409, "distribution rollback failed", "previous publication is not available", "StateError", "rollbackDistribution");
  }
  const std::string rollback_publication_id = current_pointer->at("previous_publication_id").get<std::string>();
  const fs::path publication_root = registry_root / "_publications" / rollback_publication_id;
  const fs::path publication_path = publication_root / "publication.json";
  if (!fs::exists(publication_path))
  {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}, {"publication_id", rollback_publication_id}}, "not_found");
    return FailureResponse(404, "distribution rollback failed", "previous publication is not found", "NotFound", "rollbackDistribution");
  }
  try
  {
    const nlohmann::json publication = nlohmann::json::parse(ReadFileBytes(publication_path));
    if (distribution_id == "default")
    {
      MaterializePublicationToRoot(registry_root, publication_root);
    }
    const std::string updated_at = UtcTimestampNow();
    nlohmann::json next_pointer = {
        {"distribution_id", distribution_id},
        {"publication_id", rollback_publication_id},
        {"repository_version_id", publication.at("repository_version_id").get<std::string>()},
        {"previous_publication_id", current_pointer->value("publication_id", "")},
        {"updated_at", updated_at},
    };
    WriteTextFile(registry_root / "_distributions" / distribution_id / "current.json", JsonText(next_pointer));
    RegistryDistributionRecord distribution_record{
        .distribution_id = distribution_id,
        .repository_id = publication.value("repository_id", "default"),
        .name = distribution_id,
        .base_url = RegistryReadRootUrl(config_),
        .current_publication_id = rollback_publication_id,
        .previous_publication_id = current_pointer->value("publication_id", ""),
        .updated_at = updated_at,
    };
    if (postgres_ != nullptr)
    {
      postgres_->UpsertRegistryDistribution(distribution_record);
    }
    memory_.UpsertRegistryDistribution(distribution_record);
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}, {"publication_id", rollback_publication_id}}, "success");
    return JsonResponse(200, SuccessEnvelope("rolled back distribution", next_pointer));
  }
  catch (const std::exception &error)
  {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}}, "failed");
    return FailureResponse(422, "distribution rollback failed", error.what(), "PublishError", "rollbackDistribution");
  }
}

void PlatformRouter::RecordMirrorState(
    std::string freshness,
    std::string replay_cursor,
    std::string publication_id,
    std::string repository_version_id,
    std::string synced_at,
    const int tree_size)
{
  if (postgres_ != nullptr)
  {
    postgres_->RecordMirrorState(
        config_.registry.mirror_id,
        config_.region,
        config_.registry.mirror_origin,
        freshness,
        replay_cursor,
        publication_id,
        repository_version_id,
        synced_at,
        tree_size);
  }
  memory_.RecordMirrorState(
      config_.registry.mirror_id,
      config_.region,
      config_.registry.mirror_origin,
      freshness,
      replay_cursor,
      publication_id,
      repository_version_id,
      synced_at,
      tree_size);
}

}  // namespace spio::platform
