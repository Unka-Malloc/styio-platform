#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryTrustMetadata.hpp"
#include "PlatformCore/Core/Errors.hpp"
#include "PlatformCore/Core/Sha256.hpp"
#include "PlatformCore/Manifest/Manifest.hpp"

namespace spio::platform
{

namespace
{

// Validated publish request after manifest defaults and explicit request fields
// have been merged. The draft is not persisted until artifact placement and
// index append both succeed.
struct PublishDraft
{
  std::string package;
  std::string version;
  std::string publisher_id;
  fs::path archive_path;
  int dependencies = 0;
  int dev_dependencies = 0;
};

PublishDraft
BuildPublishDraft(
  const nlohmann::json &body,
  const PlatformConfig &config,
  const std::optional<MtlsIdentity> &identity
) {
  PublishDraft draft;
  draft.publisher_id =
    body.value("publisher_id", identity.has_value() ? identity->node_id : std::string("control-plane"));

  if (HasNonEmptyString(body, "manifest_path")) {
    const spio::ManifestDocument manifest = spio::LoadManifest(body["manifest_path"].get<std::string>());
    if (!manifest.package.has_value()) {
      throw spio::ValidationError("manifest_path must point to a package manifest");
    }
    draft.package = manifest.package->name;
    draft.version = manifest.package->version;
    draft.dependencies = static_cast<int>(manifest.package->dependencies.size());
    draft.dev_dependencies = static_cast<int>(manifest.package->dev_dependencies.size());
  }

  if (HasNonEmptyString(body, "package")) {
    draft.package = body["package"].get<std::string>();
  }
  if (HasNonEmptyString(body, "version")) {
    draft.version = body["version"].get<std::string>();
  }
  if (draft.package.empty()) {
    throw spio::ValidationError("package or manifest_path is required");
  }
  if (draft.version.empty()) {
    throw spio::ValidationError("manifest_path is required when version is not provided");
  }
  if (const std::optional<std::string> error = ValidatePackageName(draft.package); error.has_value()) {
    throw spio::ValidationError(*error);
  }

  if (HasNonEmptyString(body, "archive_path")) {
    draft.archive_path = body["archive_path"].get<std::string>();
  }
  else {
    const fs::path staging_dir = fs::path(config.registry.root) / "_staging";
    draft.archive_path =
      staging_dir / (SanitizePathSegment(draft.package) + "-" + SanitizePathSegment(draft.version) + ".spio.src.tar");
    fs::create_directories(staging_dir);
    std::ofstream out(draft.archive_path);
    out << nlohmann::json{
      {"package", draft.package},
      {"version", draft.version},
      {"publisher_id", draft.publisher_id},
    }
             .dump(2)
        << "\n";
  }
  if (!fs::exists(draft.archive_path) || !fs::is_regular_file(draft.archive_path)) {
    throw spio::ValidationError("archive_path must point to a readable file");
  }
  return draft;
}

nlohmann::json
BuildReleaseRecord(
  const PublishDraft &draft,
  const std::string &published_at,
  const std::string &archive_sha256,
  const uintmax_t archive_size,
  const std::string &artifact_path
) {
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

// Result of appending one immutable release to the local static registry tree.
// The sequence maps directly to the transparency log leaf number.
struct LocalAppendResult
{
  std::string index_path;
  std::string log_leaf_path;
  size_t sequence = 0;
};

LocalAppendResult
AppendRegistryReleaseToLocal(
  const PlatformConfig &config,
  const PublishDraft &draft,
  const nlohmann::json &release_record,
  const std::string &artifact_path
) {
  const fs::path registry_root(config.registry.root);
  const fs::path artifact_dest_path = registry_root / artifact_path;
  fs::create_directories(artifact_dest_path.parent_path());
  if (fs::exists(artifact_dest_path)) {
    if (spio::Sha256File(artifact_dest_path) != release_record.at("source_artifact").at("sha256").get<std::string>()) {
      throw std::runtime_error("destination artifact already exists with different content");
    }
  }
  else {
    fs::copy_file(draft.archive_path, artifact_dest_path);
  }

  const std::string index_path = RegistryIndexPathForPackage(draft.package);
  const fs::path index_file = registry_root / index_path;
  if (fs::exists(index_file)) {
    std::ifstream in(index_file);
    std::string line;
    while (std::getline(in, line)) {
      if (JsonLineHasRelease(line, draft.package, draft.version)) {
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

}  // namespace

}  // namespace spio::platform
