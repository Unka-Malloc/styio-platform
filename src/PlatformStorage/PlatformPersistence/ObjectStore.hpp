#pragma once

#include "PlatformCore/Config.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pafio::platform
{

enum class ObjectStoreProvider
{
  S3,
  Gcs,
  Azure,
  Filesystem,
  Memory,
};

ObjectStoreProvider ParseObjectStoreProvider(std::string_view value);
std::string ToString(ObjectStoreProvider provider);
bool IsObjectStoreProviderImplemented(ObjectStoreProvider provider);
std::string BuildArtifactObjectKey(std::string_view tenant_id, std::string_view workspace_id, std::string_view job_id, std::string_view artifact_name);
std::string NormalizeObjectKey(std::string_view key);
void PutObjectBytes(const ObjectStoreConfig &config, std::string_view key, std::string_view payload, std::string_view content_type = "application/octet-stream");
void PutObjectFile(const ObjectStoreConfig &config, std::string_view key, const std::filesystem::path &path, std::string_view content_type = "application/octet-stream");
std::optional<std::string> GetObjectText(const ObjectStoreConfig &config, std::string_view key);
bool ObjectExists(const ObjectStoreConfig &config, std::string_view key);
std::vector<std::string> ListObjectKeys(const ObjectStoreConfig &config, std::string_view prefix);
nlohmann::json DescribeObjectStore(const ObjectStoreConfig &config);

}  // namespace pafio::platform
