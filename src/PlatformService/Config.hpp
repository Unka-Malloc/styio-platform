#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace spio::platform
{

struct MtlsConfig
{
  bool required = true;
  std::string ca_path;
  std::string cert_path;
  std::string key_path;
};

struct ObjectStoreConfig
{
  std::string provider = "s3";
  std::string bucket;
  std::string endpoint;
  std::string region = "local-dev";
};

struct RegistryControlConfig
{
  std::string root = ".styio-platform/registry-v2";
  std::string key_dir = ".styio-platform/registry-v2-keys";
  std::string registry_name = "spio-registry-v2";
  std::string mirror_id = "mirror-local";
  std::string mirror_origin = "registry-primary";
  std::string mirror_source_root;
};

struct WorkgroupConfig
{
  bool enabled = true;
  std::string id = "styio-local-workgroup";
  std::string trust_domain = "styio-platform-local";
  std::string registration_policy = "local-dev-default";
  std::string registration_tenant = "platform";
  std::string registration_token;
};

struct PlatformConfig
{
  std::string bind_host = "127.0.0.1";
  int bind_port = 8787;
  std::string region = "local-dev";
  std::string node_id = "node-local";
  std::vector<std::string> roles = {"control-plane", "worker", "registry-writer", "mirror"};
  std::string state_backend = "memory";
  std::string postgres_dsn;
  ObjectStoreConfig object_store;
  RegistryControlConfig registry;
  WorkgroupConfig workgroup;
  MtlsConfig mtls;
};

std::vector<std::string> SplitCsv(std::string value);
PlatformConfig LoadPlatformConfigFromEnvironment();
nlohmann::json SerializePublicConfig(const PlatformConfig &config);

}  // namespace spio::platform
