#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "PlatformService/RouterSupport.hpp"
#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"

namespace spio::platform
{

namespace
{

std::string
SanitizePathSegment(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const unsigned char ch : value) {
    if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.') {
      out.push_back(static_cast<char>(ch));
    }
    else {
      out.push_back('_');
    }
  }
  if (out.empty() || out == "." || out == "..") {
    return "_";
  }
  return out;
}

std::vector<std::string>
SplitPackageName(std::string_view package) {
  std::vector<std::string> parts;
  std::stringstream stream{std::string(package)};
  std::string item;
  while (std::getline(stream, item, '/')) {
    if (!item.empty()) {
      parts.push_back(SanitizePathSegment(item));
    }
  }
  return parts;
}

std::string
JoinPathParts(const std::vector<std::string> &parts, const size_t begin, const size_t end) {
  std::ostringstream stream;
  for (size_t index = begin; index < end; ++index) {
    if (index > begin) {
      stream << "/";
    }
    stream << parts[index];
  }
  return stream.str();
}

std::optional<std::string>
ValidatePackageName(std::string_view package) {
  const size_t slash = package.find('/');
  if (slash == std::string_view::npos || slash != package.rfind('/')) {
    return "package must use namespace/name form";
  }
  if (!IsSafeLowercaseIdentifier(package.substr(0, slash)) || !IsSafeLowercaseIdentifier(package.substr(slash + 1))) {
    return "package must use lowercase namespace/name segments";
  }
  return std::nullopt;
}

std::string
FileUrlForPath(const fs::path &path) {
  return "file://" + fs::absolute(path).lexically_normal().generic_string();
}

std::string
RegistryReadRootUrl(const PlatformConfig &config) {
  if (!config.registry.read_root_url.empty()) {
    return config.registry.read_root_url;
  }
  if (ParseObjectStoreProvider(config.object_store.provider) == ObjectStoreProvider::S3 && !config.object_store.endpoint.empty() && !config.object_store.bucket.empty() && config.object_store.path_style) {
    std::string root = config.object_store.endpoint;
    while (!root.empty() && root.back() == '/') {
      root.pop_back();
    }
    root += "/" + config.object_store.bucket;
    std::string prefix = config.object_store.prefix;
    while (!prefix.empty() && prefix.front() == '/') {
      prefix.erase(prefix.begin());
    }
    while (!prefix.empty() && prefix.back() == '/') {
      prefix.pop_back();
    }
    if (!prefix.empty()) {
      root += "/" + prefix;
    }
    return root;
  }
  return FileUrlForPath(config.registry.root);
}

std::string
RegistryControlPlaneBaseUrl(const PlatformConfig &config) {
  if (!config.registry.control_plane_base_url.empty()) {
    return config.registry.control_plane_base_url;
  }
  return "/api/spio-registry-control/v1";
}

std::string
RegistryIndexPathForPackage(std::string_view package) {
  const std::vector<std::string> parts = SplitPackageName(package);
  return "index/" + JoinPathParts(parts, 0, parts.size() - 1) + "/" + parts.back() + ".jsonl";
}

std::string
RegistryReleaseKey(const std::string &package, const std::string &version) {
  return package + "@" + version;
}

std::string
RegistryPackageId(std::string_view package) {
  return std::string(package);
}

std::string
RegistryPackageFromRoute(const RouteMatch &match) {
  return match.parameters.at("namespace") + "/" + match.parameters.at("name");
}

std::string
RegistryActorId(const HttpRequest &request) {
  if (request.identity.has_value()) {
    return request.identity->node_id;
  }
  if (const std::optional<std::string> token = RegistryTokenFromHeaders(request.headers); token.has_value()) {
    const std::string hash = Sha256Bytes(*token);
    return "token:" + hash.substr(0, 12);
  }
  return "anonymous";
}

std::vector<std::string>
JsonStringArray(const nlohmann::json &body, const std::string &field, std::vector<std::string> fallback) {
  if (!body.contains(field)) {
    return fallback;
  }
  std::vector<std::string> values;
  for (const nlohmann::json &entry : body.at(field)) {
    values.push_back(entry.get<std::string>());
  }
  return values;
}

bool
PackagePatternMatches(std::string_view pattern, std::string_view package) {
  if (pattern == "*") {
    return true;
  }
  if (pattern.ends_with("/*")) {
    const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
    return package.starts_with(prefix);
  }
  return pattern == package;
}

bool
TokenHasScope(const RegistryPublishTokenRecord &token, std::string_view scope) {
  return std::find(token.scopes.begin(), token.scopes.end(), scope) != token.scopes.end();
}

bool
TokenMatchesPackage(const RegistryPublishTokenRecord &token, const std::string &package) {
  if (package.empty()) {
    return true;
  }
  return std::any_of(token.package_patterns.begin(), token.package_patterns.end(), [&](const std::string &pattern)
                     {
                       return PackagePatternMatches(pattern, package);
                     });
}

bool
VersionLess(const std::string &left, const std::string &right) {
  auto parse = [](const std::string &version)
  {
    std::array<int, 3> numeric = {0, 0, 0};
    std::string suffix;
    std::stringstream stream(version);
    std::string part;
    size_t index = 0;
    while (std::getline(stream, part, '.') && index < numeric.size()) {
      try {
        numeric[index] = std::stoi(part);
      }
      catch (...) {
        suffix = version;
        break;
      }
      ++index;
    }
    if (index != numeric.size() || stream.good()) {
      suffix = version;
    }
    return std::tuple<int, int, int, std::string>(numeric[0], numeric[1], numeric[2], suffix);
  };
  return parse(left) < parse(right);
}

}  // namespace

}  // namespace spio::platform
