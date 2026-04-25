#include "PlatformService/Config.hpp"

#include <cstdlib>
#include <sstream>

namespace spio::platform
{

namespace
{

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

bool EnvBool(const char *name, bool fallback)
{
  const std::string value = EnvString(name, fallback ? "1" : "0");
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

}  // namespace

std::vector<std::string> SplitCsv(std::string value)
{
  std::vector<std::string> entries;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ','))
  {
    const auto begin = item.find_first_not_of(" \t\n\r");
    const auto end = item.find_last_not_of(" \t\n\r");
    if (begin == std::string::npos)
    {
      continue;
    }
    entries.push_back(item.substr(begin, end - begin + 1));
  }
  return entries;
}

PlatformConfig LoadPlatformConfigFromEnvironment()
{
  PlatformConfig config;
  config.bind_host = EnvString("STYIO_PLATFORM_BIND_HOST", config.bind_host);
  config.bind_port = EnvInt("STYIO_PLATFORM_BIND_PORT", config.bind_port);
  config.region = EnvString("STYIO_PLATFORM_REGION", config.region);
  config.node_id = EnvString("STYIO_PLATFORM_NODE_ID", config.node_id);
  const std::vector<std::string> roles = SplitCsv(EnvString("STYIO_PLATFORM_NODE_ROLES", ""));
  if (!roles.empty())
  {
    config.roles = roles;
  }
  config.postgres_dsn = EnvString("STYIO_PLATFORM_POSTGRES_DSN", "");
  config.object_store.provider = EnvString("STYIO_PLATFORM_OBJECT_STORE_PROVIDER", config.object_store.provider);
  config.object_store.bucket = EnvString("STYIO_PLATFORM_OBJECT_STORE_BUCKET", "");
  config.object_store.endpoint = EnvString("STYIO_PLATFORM_OBJECT_STORE_ENDPOINT", "");
  config.object_store.region = EnvString("STYIO_PLATFORM_OBJECT_STORE_REGION", config.region);
  config.mtls.required = EnvBool("STYIO_PLATFORM_MTLS_REQUIRED", config.mtls.required);
  config.mtls.ca_path = EnvString("STYIO_PLATFORM_MTLS_CA", "");
  config.mtls.cert_path = EnvString("STYIO_PLATFORM_MTLS_CERT", "");
  config.mtls.key_path = EnvString("STYIO_PLATFORM_MTLS_KEY", "");
  return config;
}

nlohmann::json SerializePublicConfig(const PlatformConfig &config)
{
  return {
      {"bind_host", config.bind_host},
      {"bind_port", config.bind_port},
      {"region", config.region},
      {"node_id", config.node_id},
      {"roles", config.roles},
      {"postgres_configured", !config.postgres_dsn.empty()},
      {"object_store_provider", config.object_store.provider},
      {"object_store_bucket_configured", !config.object_store.bucket.empty()},
      {"object_store_endpoint_configured", !config.object_store.endpoint.empty()},
      {"mtls_required", config.mtls.required},
      {"mtls_ca_configured", !config.mtls.ca_path.empty()},
      {"mtls_cert_configured", !config.mtls.cert_path.empty()},
      {"mtls_key_configured", !config.mtls.key_path.empty()},
  };
}

}  // namespace spio::platform
