#pragma once

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryIndexStore.hpp"
#include "PlatformCore/Core/Process.hpp"
#include "PlatformCore/Core/Sha256.hpp"

namespace spio::platform
{

namespace
{

std::string
SecureRandomHex(const size_t bytes) {
  std::vector<unsigned char> random(bytes);
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    throw std::runtime_error("secure random generation failed");
  }
  return HexBytes(random.data(), random.size());
}

std::string
Base64Encode(std::string_view payload) {
  std::string encoded(4U * ((payload.size() + 2U) / 3U), '\0');
  const int written = EVP_EncodeBlock(
    reinterpret_cast<unsigned char *>(encoded.data()),
    reinterpret_cast<const unsigned char *>(payload.data()),
    static_cast<int>(payload.size())
  );
  if (written < 0) {
    throw std::runtime_error("base64 encoding failed");
  }
  encoded.resize(static_cast<size_t>(written));
  return encoded;
}

spio::ProcessResult
RunRegistryOpenSsl(
  std::vector<std::string> args,
  std::string input = {},
  const size_t max_stdout_bytes = 1U << 20
) {
  spio::ProcessResult result = spio::RunProcess({
    .program = "openssl",
    .args = std::move(args),
    .timeout = std::chrono::seconds{30},
    .max_stdout_bytes = max_stdout_bytes,
    .max_stderr_bytes = 1U << 20,
    .stdin_text = std::move(input),
    .error_context = "registry v2 openssl command",
  });
  if (result.exit_code != 0 || result.timed_out) {
    throw std::runtime_error("registry v2 openssl command failed: " + spio::DescribeProcessFailure(result));
  }
  return result;
}

// One signing key for a registry trust role. Private-key paths stay local; only
// public keys and signatures are published into the static read plane.
struct RegistryRoleKey
{
  std::string role;
  std::string keyid;
  fs::path private_key_path;
  fs::path public_key_path;
  std::string public_key_pem;
};

const std::vector<std::string> &
RegistryRoleNames() {
  static const std::vector<std::string> roles = {"root", "timestamp", "snapshot", "targets", "log"};
  return roles;
}

std::string
RegistryFileKeyId(const fs::path &public_key_path) {
  const spio::ProcessResult der = RunRegistryOpenSsl(
    {"pkey", "-pubin", "-in", public_key_path.string(), "-outform", "DER"},
    {},
    64U << 10
  );
  return Sha256Bytes(der.stdout_text);
}

void
GenerateRegistryKeyDirectory(const fs::path &key_dir) {
  fs::create_directories(key_dir / "private");
  fs::create_directories(key_dir / "public");
  nlohmann::json roles = nlohmann::json::object();
  for (const std::string &role : RegistryRoleNames()) {
    const fs::path private_key = key_dir / "private" / (role + ".pem");
    const fs::path public_key = key_dir / "public" / (role + ".pem");
    if (!fs::exists(private_key) || !fs::exists(public_key)) {
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
    })
  );
}

std::map<std::string, RegistryRoleKey>
LoadOrCreateRegistryRoleKeys(const fs::path &key_dir) {
  if (!fs::exists(key_dir / "keys.json")) {
    GenerateRegistryKeyDirectory(key_dir);
  }
  const nlohmann::json manifest = nlohmann::json::parse(ReadFileBytes(key_dir / "keys.json"));
  std::map<std::string, RegistryRoleKey> loaded;
  for (const std::string &role : RegistryRoleNames()) {
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

nlohmann::json
RegistryRoleKeysPayload(const std::map<std::string, RegistryRoleKey> &role_keys) {
  nlohmann::json keys = nlohmann::json::object();
  for (const auto &[role, key] : role_keys) {
    (void)role;
    keys[key.keyid] = {
      {"keytype", "ed25519"},
      {"scheme", "ed25519"},
      {"keyval", {{"public", key.public_key_pem}}},
    };
  }
  return keys;
}

nlohmann::json
RegistryRolesPolicyPayload(const std::map<std::string, RegistryRoleKey> &role_keys) {
  nlohmann::json roles = nlohmann::json::object();
  for (const std::string &role : RegistryRoleNames()) {
    roles[role] = {
      {"keyids", {role_keys.at(role).keyid}},
      {"threshold", 1},
    };
  }
  return roles;
}

nlohmann::json
SignedRegistryPayload(
  const nlohmann::json &signed_payload,
  const RegistryRoleKey &role_key,
  const fs::path &temp_root
) {
  fs::create_directories(temp_root);
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path message_path = temp_root / (role_key.role + "-" + std::to_string(stamp) + ".json");
  const fs::path signature_path = temp_root / (role_key.role + "-" + std::to_string(stamp) + ".sig");
  WriteTextFile(message_path, CanonicalJson(signed_payload));
  try {
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
  catch (...) {
    std::error_code ec;
    fs::remove(message_path, ec);
    fs::remove(signature_path, ec);
    throw;
  }
}

nlohmann::json
RegistryConfigPayload(const PlatformConfig &config, const std::string &generated_at) {
  return {
    {"schema_version", 1},
    {"protocol", "pafio-static-registry"},
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

nlohmann::json
RegistryConfigPayload(
  const PlatformConfig &config,
  const std::string &generated_at,
  const std::string &publication_id,
  const std::string &repository_version_id
) {
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

nlohmann::json
SignedFileMeta(const fs::path &path, const int version) {
  return {
    {"version", version},
    {"length", static_cast<int64_t>(fs::file_size(path))},
    {"hashes", {{"sha256", spio::Sha256File(path)}}},
  };
}

int
ReadSignedVersion(const fs::path &path) {
  if (!fs::exists(path)) {
    return 0;
  }
  try {
    return nlohmann::json::parse(ReadFileBytes(path)).at("signed").value("version", 0);
  }
  catch (...) {
    return 0;
  }
}

std::vector<fs::path>
LeafSequencePaths(const fs::path &root) {
  std::vector<fs::path> paths;
  const fs::path leaves_root = root / "log" / "leaves";
  std::error_code ec;
  if (!fs::exists(leaves_root, ec)) {
    return paths;
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(leaves_root, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string
TransparencyRootHash(const std::vector<std::string> &leaf_hashes) {
  std::string state(32, '\0');
  for (const std::string &leaf_hash : leaf_hashes) {
    std::string leaf_bytes;
    leaf_bytes.reserve(32);
    for (size_t index = 0; index + 1 < leaf_hash.size(); index += 2) {
      leaf_bytes.push_back(static_cast<char>(std::stoi(leaf_hash.substr(index, 2), nullptr, 16)));
    }
    const std::string combined = state + leaf_bytes;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(combined.data()), combined.size(), digest);
    state.assign(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH);
  }
  return HexBytes(reinterpret_cast<const unsigned char *>(state.data()), state.size());
}

struct PackageMaps
{
  nlohmann::json namespace_packages = nlohmann::json::object();
  nlohmann::json snapshot_meta = nlohmann::json::object();
};

PackageMaps
CollectPackageMaps(const fs::path &registry_root) {
  PackageMaps result;
  const fs::path index_root = registry_root / "index";
  std::error_code ec;
  if (!fs::exists(index_root, ec)) {
    return result;
  }
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(index_root, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".jsonl") {
      continue;
    }
    std::ifstream in(entry.path());
    std::vector<nlohmann::json> records;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty()) {
        records.push_back(nlohmann::json::parse(line));
      }
    }
    if (records.empty()) {
      throw std::runtime_error("registry index file is empty: " + entry.path().string());
    }
    const std::string package_name = records.front().at("package").get<std::string>();
    const std::string package_namespace = SplitPackageName(package_name).front();
    std::vector<std::string> versions;
    nlohmann::json releases = nlohmann::json::object();
    for (const nlohmann::json &record : records) {
      if (record.at("package").get<std::string>() != package_name) {
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

// Snapshot of metadata versions regenerated for one publish/yank transaction.
struct MetadataVersions
{
  int checkpoint_version = 0;
  int snapshot_version = 0;
  int timestamp_version = 0;
  size_t namespaces = 0;
};

MetadataVersions
RefreshSignedRegistryMetadata(
  const PlatformConfig &config,
  const std::map<std::string, RegistryRoleKey> &role_keys,
  const std::string &registry_time
) {
  const fs::path registry_root(config.registry.root);
  const fs::path temp_root = registry_root / "_tmp";
  PackageMaps package_maps = CollectPackageMaps(registry_root);
  for (auto &[namespace_name, package_map] : package_maps.namespace_packages.items()) {
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
  for (const fs::path &leaf_path : LeafSequencePaths(registry_root)) {
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

void
EnsureRegistryRootInitialized(const PlatformConfig &config) {
  const fs::path root(config.registry.root);
  fs::create_directories(root);
  const fs::path key_dir(config.registry.key_dir);
  fs::create_directories(key_dir);
  const std::map<std::string, RegistryRoleKey> role_keys = LoadOrCreateRegistryRoleKeys(key_dir);
  const fs::path temp_root = root / "_tmp";
  const std::string registry_time = UtcTimestampNow();

  if (!fs::exists(root / "config.json")) {
    WriteTextFile(root / "config.json", JsonText(RegistryConfigPayload(config, registry_time)));
  }
  if (!fs::exists(root / "log" / "checkpoint.json")) {
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
  if (!fs::exists(root / "trust" / "snapshot.json")) {
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
  if (!fs::exists(root / "trust" / "timestamp.json")) {
    const nlohmann::json timestamp_signed = {
      {"type", "timestamp"},
      {"spec_version", "1"},
      {"version", 1},
      {"expires", UtcTimestampPlusDays(1)},
      {"meta", {{"trust/snapshot.json", SignedFileMeta(root / "trust" / "snapshot.json", 1)}}},
    };
    WriteTextFile(root / "trust" / "timestamp.json", JsonText(SignedRegistryPayload(timestamp_signed, role_keys.at("timestamp"), temp_root)));
  }
  if (!fs::exists(root / "trust" / "root.json")) {
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

}  // namespace

}  // namespace spio::platform
