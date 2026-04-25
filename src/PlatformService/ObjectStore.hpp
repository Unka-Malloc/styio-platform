#pragma once

#include "PlatformService/Config.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace spio::platform
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
nlohmann::json DescribeObjectStore(const ObjectStoreConfig &config);

}  // namespace spio::platform
