#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryIndexStore.hpp"
#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"

namespace spio::platform
{

namespace
{

bool
UsesS3ObjectStore(const PlatformConfig &config) {
  return ParseObjectStoreProvider(config.object_store.provider) == ObjectStoreProvider::S3;
}

void
RemoveLocalRegistryMetadataCache(const PlatformConfig &config) {
  const fs::path root(config.registry.root);
  std::error_code ec;
  fs::remove(root / "config.json", ec);
  fs::remove_all(root / "trust", ec);
  fs::remove_all(root / "index", ec);
  fs::remove_all(root / "log", ec);
  fs::remove_all(root / "artifacts", ec);
}

void
SyncS3RegistryStateToLocal(const PlatformConfig &config) {
  RemoveLocalRegistryMetadataCache(config);
  const fs::path root(config.registry.root);
  if (const std::optional<std::string> config_text = GetObjectText(config.object_store, "config.json"); config_text.has_value()) {
    WriteTextFile(root / "config.json", *config_text);
  }
  for (const std::string &prefix : {"trust/", "index/", "log/", "artifacts/", "_publications/", "_distributions/"}) {
    for (const std::string &key : ListObjectKeys(config.object_store, prefix)) {
      if (key.starts_with("_staging/") || key.starts_with("_tmp/")) {
        continue;
      }
      if (const std::optional<std::string> payload = GetObjectText(config.object_store, key); payload.has_value()) {
        WriteTextFile(root / NormalizeObjectKey(key), *payload);
      }
    }
  }
}

std::string
RegistryContentTypeForPath(const std::string &relative_path) {
  if (relative_path.ends_with(".json")) {
    return "application/json";
  }
  if (relative_path.ends_with(".jsonl")) {
    return "application/x-ndjson";
  }
  return "application/octet-stream";
}

void
UploadRegistryTreeToS3(const PlatformConfig &config) {
  const fs::path root(config.registry.root);
  std::error_code ec;
  if (!fs::exists(root, ec)) {
    return;
  }
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string relative = fs::relative(entry.path(), root).generic_string();
    if (relative.starts_with("_staging/") || relative.starts_with("_tmp/")) {
      continue;
    }
    PutObjectFile(config.object_store, relative, entry.path(), RegistryContentTypeForPath(relative));
  }
}

}  // namespace

}  // namespace spio::platform
