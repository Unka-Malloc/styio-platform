#include "PlatformService/Router.hpp"

#include "PlatformService/ObjectStore.hpp"
#include "PlatformService/PostgresStore.hpp"
#include "SpioCore/Errors.hpp"
#include "SpioCore/Process.hpp"
#include "SpioCore/Sha256.hpp"
#include "SpioManifest/Manifest.hpp"

#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <system_error>
#include <tuple>

#include <openssl/evp.h>
#include <openssl/sha.h>

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
  return operation_id == "registryStatus" || operation_id == "registryDescriptor" ||
         operation_id == "publishRelease" || operation_id == "verifyRegistry";
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
  if (operation_id == "registryDescriptor")
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
  for (const std::string &prefix : {"trust/", "index/", "log/"})
  {
    for (const std::string &key : ListObjectKeys(config.object_store, prefix))
    {
      if (key.starts_with("artifacts/") || key.starts_with("_"))
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
          .operation_id = "registryDescriptor",
          .method = HttpMethod::Get,
          .path = "/api/spio-registry-control/v1/descriptor",
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
    job_id = NextMemoryJobId();
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
  const auto found = jobs_.find(match.parameters.at("job_id"));
  if (found == jobs_.end())
  {
    return JsonResponse(404, FailureEnvelope("job cancellation failed", "job not found", "NotFound", "cancelJob"));
  }
  PlatformJobRecord &job = found->second;
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
  const auto found = jobs_.find(match.parameters.at("job_id"));
  if (found == jobs_.end())
  {
    return JsonResponse(404, FailureEnvelope("job heartbeat failed", "job not found", "NotFound", "heartbeatJob"));
  }
  PlatformJobRecord &job = found->second;
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
  const auto found = jobs_.find(match.parameters.at("job_id"));
  if (found == jobs_.end())
  {
    return JsonResponse(404, FailureEnvelope("job completion failed", "job not found", "NotFound", "completeJob"));
  }
  PlatformJobRecord &job = found->second;
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
    };
    published_releases_[release_key] = payload;
    RecordMirrorState("fresh", "checkpoint-" + PaddedNumber(append_result.sequence, 4));
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

std::string PlatformRouter::NextMemoryJobId()
{
  return "job-" + PaddedNumber(next_memory_job_sequence_++, 12);
}

}  // namespace spio::platform
