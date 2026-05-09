#include "PlatformCore/Config.hpp"

#include "PlatformCore/System/OperatingSystemAdapter.hpp"

#include <sstream>

namespace spio::platform
{

namespace
{

std::string EnvString(const char *name, std::string fallback)
{
  const std::optional<std::string> value = DefaultOperatingSystemAdapter().GetEnv(name);
  if (!value.has_value() || value->empty())
  {
    return fallback;
  }
  return *value;
}

int EnvInt(const char *name, int fallback)
{
  const std::optional<std::string> value = DefaultOperatingSystemAdapter().GetEnv(name);
  if (!value.has_value() || value->empty())
  {
    return fallback;
  }
  try
  {
    return std::stoi(*value);
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
  config.state_backend = EnvString("STYIO_PLATFORM_STATE_BACKEND", config.state_backend);
  config.postgres_dsn = EnvString("STYIO_PLATFORM_POSTGRES_DSN", "");
  config.object_store.provider = EnvString("STYIO_PLATFORM_OBJECT_STORE_PROVIDER", config.object_store.provider);
  config.object_store.bucket = EnvString("STYIO_PLATFORM_OBJECT_STORE_BUCKET", "");
  config.object_store.endpoint = EnvString("STYIO_PLATFORM_OBJECT_STORE_ENDPOINT", "");
  config.object_store.region = EnvString("STYIO_PLATFORM_OBJECT_STORE_REGION", config.region);
  config.object_store.access_key_id = EnvString("STYIO_PLATFORM_OBJECT_STORE_ACCESS_KEY_ID", "");
  config.object_store.secret_access_key = EnvString("STYIO_PLATFORM_OBJECT_STORE_SECRET_ACCESS_KEY", "");
  config.object_store.session_token = EnvString("STYIO_PLATFORM_OBJECT_STORE_SESSION_TOKEN", "");
  config.object_store.prefix = EnvString("STYIO_PLATFORM_OBJECT_STORE_PREFIX", "");
  config.object_store.path_style = EnvBool("STYIO_PLATFORM_OBJECT_STORE_PATH_STYLE", config.object_store.path_style);
  config.registry.root = EnvString("STYIO_PLATFORM_REGISTRY_ROOT", config.registry.root);
  config.registry.key_dir = EnvString("STYIO_PLATFORM_REGISTRY_KEY_DIR", config.registry.key_dir);
  config.registry.registry_name = EnvString("STYIO_PLATFORM_REGISTRY_NAME", config.registry.registry_name);
  config.registry.read_root_url = EnvString("STYIO_PLATFORM_REGISTRY_READ_ROOT_URL", config.registry.read_root_url);
  config.registry.control_plane_base_url =
      EnvString("STYIO_PLATFORM_REGISTRY_CONTROL_PLANE_BASE_URL", config.registry.control_plane_base_url);
  config.registry.mirror_id = EnvString("STYIO_PLATFORM_REGISTRY_MIRROR_ID", config.registry.mirror_id);
  config.registry.mirror_origin = EnvString("STYIO_PLATFORM_REGISTRY_MIRROR_ORIGIN", config.registry.mirror_origin);
  config.registry.mirror_source_root = EnvString("STYIO_PLATFORM_REGISTRY_MIRROR_SOURCE_ROOT", config.registry.mirror_source_root);
  config.workgroup.enabled = EnvBool("STYIO_PLATFORM_WORKGROUP_ENABLED", config.workgroup.enabled);
  config.workgroup.id = EnvString("STYIO_PLATFORM_WORKGROUP_ID", config.workgroup.id);
  config.workgroup.trust_domain = EnvString("STYIO_PLATFORM_WORKGROUP_TRUST_DOMAIN", config.workgroup.trust_domain);
  config.workgroup.registration_policy =
      EnvString("STYIO_PLATFORM_WORKGROUP_REGISTRATION_POLICY", config.workgroup.registration_policy);
  config.workgroup.registration_tenant =
      EnvString("STYIO_PLATFORM_WORKGROUP_REGISTRATION_TENANT", config.workgroup.registration_tenant);
  config.workgroup.registration_token = EnvString("STYIO_PLATFORM_WORKGROUP_REGISTRATION_TOKEN", "");
  config.mtls.tls_enabled = EnvBool("STYIO_PLATFORM_TLS_ENABLED", config.mtls.tls_enabled);
  config.mtls.required = EnvBool("STYIO_PLATFORM_MTLS_REQUIRED", config.mtls.required);
  config.mtls.trust_proxy_identity_headers =
      EnvBool("STYIO_PLATFORM_TRUST_PROXY_IDENTITY_HEADERS", !config.mtls.tls_enabled);
  config.mtls.auto_provision = EnvBool("STYIO_PLATFORM_MTLS_AUTO_PROVISION", config.mtls.auto_provision);
  config.mtls.ca_root = EnvString("STYIO_PLATFORM_MTLS_CA_ROOT", config.mtls.ca_root);
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
      {"state_backend", config.state_backend},
      {"postgres_configured", !config.postgres_dsn.empty()},
      {"object_store_provider", config.object_store.provider},
      {"object_store_bucket_configured", !config.object_store.bucket.empty()},
      {"object_store_endpoint_configured", !config.object_store.endpoint.empty()},
      {"object_store_access_key_configured", !config.object_store.access_key_id.empty()},
      {"object_store_secret_configured", !config.object_store.secret_access_key.empty()},
      {"object_store_prefix_configured", !config.object_store.prefix.empty()},
      {"object_store_path_style", config.object_store.path_style},
      {"registry_root_configured", !config.registry.root.empty()},
      {"registry_key_dir_configured", !config.registry.key_dir.empty()},
      {"registry_name", config.registry.registry_name},
      {"registry_read_root_url_configured", !config.registry.read_root_url.empty()},
      {"registry_control_plane_base_url_configured", !config.registry.control_plane_base_url.empty()},
      {"registry_mirror_id", config.registry.mirror_id},
      {"registry_mirror_origin", config.registry.mirror_origin},
      {"registry_mirror_source_root_configured", !config.registry.mirror_source_root.empty()},
      {"workgroup_enabled", config.workgroup.enabled},
      {"workgroup_id", config.workgroup.id},
      {"workgroup_trust_domain", config.workgroup.trust_domain},
      {"workgroup_registration_policy", config.workgroup.registration_policy},
      {"workgroup_registration_tenant", config.workgroup.registration_tenant},
      {"workgroup_registration_token_configured", !config.workgroup.registration_token.empty()},
      {"tls_enabled", config.mtls.tls_enabled},
      {"mtls_required", config.mtls.required},
      {"trust_proxy_identity_headers", config.mtls.trust_proxy_identity_headers},
      {"mtls_auto_provision", config.mtls.auto_provision},
      {"mtls_ca_root_configured", !config.mtls.ca_root.empty()},
      {"mtls_ca_configured", !config.mtls.ca_path.empty()},
      {"mtls_cert_configured", !config.mtls.cert_path.empty()},
      {"mtls_key_configured", !config.mtls.key_path.empty()},
  };
}

}  // namespace spio::platform
