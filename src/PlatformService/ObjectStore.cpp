#include "PlatformService/ObjectStore.hpp"

#include <algorithm>

namespace spio::platform
{

namespace
{

std::string SanitizeKeyPart(std::string_view value)
{
  std::string result;
  result.reserve(value.size());
  for (const char ch : value)
  {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
    {
      result.push_back(ch);
    }
    else
    {
      result.push_back('_');
    }
  }
  if (result.empty())
  {
    return "unset";
  }
  return result;
}

}  // namespace

ObjectStoreProvider ParseObjectStoreProvider(std::string_view value)
{
  if (value == "gcs")
  {
    return ObjectStoreProvider::Gcs;
  }
  if (value == "azure")
  {
    return ObjectStoreProvider::Azure;
  }
  if (value == "filesystem")
  {
    return ObjectStoreProvider::Filesystem;
  }
  if (value == "memory")
  {
    return ObjectStoreProvider::Memory;
  }
  return ObjectStoreProvider::S3;
}

std::string ToString(ObjectStoreProvider provider)
{
  switch (provider)
  {
    case ObjectStoreProvider::S3:
      return "s3";
    case ObjectStoreProvider::Gcs:
      return "gcs";
    case ObjectStoreProvider::Azure:
      return "azure";
    case ObjectStoreProvider::Filesystem:
      return "filesystem";
    case ObjectStoreProvider::Memory:
      return "memory";
  }
  return "s3";
}

bool IsObjectStoreProviderImplemented(ObjectStoreProvider provider)
{
  return provider == ObjectStoreProvider::S3 || provider == ObjectStoreProvider::Filesystem || provider == ObjectStoreProvider::Memory;
}

std::string BuildArtifactObjectKey(std::string_view tenant_id, std::string_view workspace_id, std::string_view job_id, std::string_view artifact_name)
{
  return "tenants/" + SanitizeKeyPart(tenant_id) +
         "/workspaces/" + SanitizeKeyPart(workspace_id) +
         "/jobs/" + SanitizeKeyPart(job_id) +
         "/artifacts/" + SanitizeKeyPart(artifact_name);
}

nlohmann::json DescribeObjectStore(const ObjectStoreConfig &config)
{
  const ObjectStoreProvider provider = ParseObjectStoreProvider(config.provider);
  return {
      {"provider", ToString(provider)},
      {"implemented", IsObjectStoreProviderImplemented(provider)},
      {"bucket_configured", !config.bucket.empty()},
      {"endpoint_configured", !config.endpoint.empty()},
      {"region", config.region},
  };
}

}  // namespace spio::platform
