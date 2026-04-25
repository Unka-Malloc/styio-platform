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

struct PlatformConfig
{
  std::string bind_host = "127.0.0.1";
  int bind_port = 8787;
  std::string region = "local-dev";
  std::string node_id = "node-local";
  std::vector<std::string> roles = {"control-plane", "worker", "registry-writer", "mirror"};
  std::string postgres_dsn;
  ObjectStoreConfig object_store;
  MtlsConfig mtls;
};

std::vector<std::string> SplitCsv(std::string value);
PlatformConfig LoadPlatformConfigFromEnvironment();
nlohmann::json SerializePublicConfig(const PlatformConfig &config);

}  // namespace spio::platform
