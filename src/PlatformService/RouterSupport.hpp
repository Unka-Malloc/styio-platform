#pragma once

#include <openssl/sha.h>

#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "PlatformSecurity/PlatformClientAuth/Authorization.hpp"
#include "PlatformService/RouteCatalog.hpp"
#include "PlatformService/Router.hpp"

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

std::optional<std::string>
RegistryTokenFromHeaders(const std::map<std::string, std::string> &headers) {
  if (const auto direct = headers.find("x-styio-registry-token"); direct != headers.end() && !direct->second.empty()) {
    return direct->second;
  }
  const auto authorization = headers.find("authorization");
  if (authorization == headers.end()) {
    return std::nullopt;
  }
  constexpr std::string_view prefix = "Bearer ";
  if (!authorization->second.starts_with(prefix)) {
    return std::nullopt;
  }
  return authorization->second.substr(prefix.size());
}

bool
UsesPostgresState(const PlatformConfig &config) {
  return config.state_backend == "postgres";
}

HttpResponse
JsonResponse(int status, nlohmann::json body) {
  return {.status_code = status, .body = std::move(body)};
}

nlohmann::json
RegistryFailureEnvelope(
  std::string message,
  std::string detail,
  std::string category,
  int returncode = 17
) {
  return {
    {"returncode", returncode},
    {"message", std::move(message)},
    {"stdout", ""},
    {"stderr", detail},
    {"error_payload", {
                        {"category", std::move(category)},
                        {"detail", std::move(detail)},
                      }},
  };
}

HttpResponse
FailureResponse(
  int status,
  std::string message,
  std::string detail,
  std::string category,
  std::string operation_id,
  int returncode = 17
) {
  if (IsRegistryOperation(operation_id)) {
    return JsonResponse(
      status,
      RegistryFailureEnvelope(std::move(message), std::move(detail), std::move(category), returncode)
    );
  }
  return JsonResponse(
    status,
    FailureEnvelope(std::move(message), std::move(detail), std::move(category), std::move(operation_id), returncode)
  );
}

bool
HasNonEmptyString(const nlohmann::json &body, const std::string &field) {
  return body.contains(field) && body[field].is_string() && !body[field].get<std::string>().empty();
}

std::optional<std::string>
ValidateOptionalStringFields(
  const nlohmann::json &body,
  std::initializer_list<std::string_view> fields
) {
  for (const std::string_view field : fields) {
    const std::string key(field);
    if (body.contains(key) && (!body[key].is_string() || body[key].get<std::string>().empty())) {
      return key + " must be a non-empty string when present";
    }
  }
  return std::nullopt;
}

std::string
PaddedNumber(const size_t value, const int width) {
  std::ostringstream stream;
  stream << std::setw(width) << std::setfill('0') << value;
  return stream.str();
}

bool
IsSafeLowercaseIdentifier(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!((first >= 'a' && first <= 'z') || std::isdigit(first) != 0)) {
    return false;
  }
  for (const unsigned char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || std::isdigit(ch) != 0 || ch == '-' || ch == '_')) {
      return false;
    }
  }
  return true;
}

bool
IsSafeWorkgroupId(std::string_view value) {
  return IsSafeLowercaseIdentifier(value);
}

bool
LooksLikeHttpEndpoint(std::string_view value) {
  return value.starts_with("http://") || value.starts_with("https://");
}

std::optional<std::string>
ValidateStringArray(const nlohmann::json &body, const std::string &field) {
  if (!body.contains(field)) {
    return std::nullopt;
  }
  if (!body[field].is_array()) {
    return field + " must be an array";
  }
  for (const nlohmann::json &entry : body[field]) {
    if (!entry.is_string() || entry.get<std::string>().empty()) {
      return field + " entries must be non-empty strings";
    }
  }
  return std::nullopt;
}

nlohmann::json
WorkgroupPolicyPayload(const PlatformConfig &config) {
  return {
    {"enabled", config.workgroup.enabled},
    {"workgroup_id", config.workgroup.id},
    {"trust_domain", config.workgroup.trust_domain},
    {"registration_policy", config.workgroup.registration_policy},
    {"registration_tenant", config.workgroup.registration_tenant},
    {"registration_token_required", !config.workgroup.registration_token.empty()},
    {"allowed_registration_roles", {"operator", "control-plane", "cluster-registrar"}},
    {"allowed_read_roles", {"operator", "control-plane", "cluster-registrar", "worker", "mirror", "registry-writer"}},
  };
}

std::optional<std::string>
ValidateWorkgroupClusterRegistration(
  const std::string &workgroup_id,
  const nlohmann::json &body
) {
  if (!IsSafeWorkgroupId(workgroup_id)) {
    return "workgroup_id must start with a lowercase letter or digit and contain only lowercase letters, digits, hyphen, or underscore";
  }
  for (const std::string field : {"cluster_id", "region", "node_id", "control_plane_endpoint"}) {
    if (!HasNonEmptyString(body, field)) {
      return field + " is required";
    }
  }
  if (!IsSafeWorkgroupId(body["cluster_id"].get<std::string>())) {
    return "cluster_id must start with a lowercase letter or digit and contain only lowercase letters, digits, hyphen, or underscore";
  }
  if (!LooksLikeHttpEndpoint(body["control_plane_endpoint"].get<std::string>())) {
    return "control_plane_endpoint must be an http or https endpoint";
  }
  for (const std::string field : {"registry_endpoint", "mirror_endpoint", "internal_control_plane_endpoint"}) {
    if (HasNonEmptyString(body, field) && !LooksLikeHttpEndpoint(body[field].get<std::string>())) {
      return field + " must be an http or https endpoint";
    }
  }
  if (const std::optional<std::string> error = ValidateStringArray(body, "roles"); error.has_value()) {
    return error;
  }
  if (body.contains("labels") && !body["labels"].is_object()) {
    return "labels must be an object when present";
  }
  return std::nullopt;
}

nlohmann::json
BuildWorkgroupClusterRecord(
  const std::string &workgroup_id,
  const nlohmann::json &body,
  const PlatformConfig &config,
  const std::optional<MtlsIdentity> &identity
) {
  nlohmann::json roles = body.value("roles", config.roles);
  nlohmann::json record = {
    {"workgroup_id", workgroup_id},
    {"cluster_id", body["cluster_id"].get<std::string>()},
    {"region", body["region"].get<std::string>()},
    {"node_id", body["node_id"].get<std::string>()},
    {"control_plane_endpoint", body["control_plane_endpoint"].get<std::string>()},
    {"roles", std::move(roles)},
    {"labels", body.value("labels", nlohmann::json::object())},
    {"trust_domain", body.value("trust_domain", config.workgroup.trust_domain)},
    {"registration_policy", config.workgroup.registration_policy},
    {"status", "registered"},
    {"registered_by", identity.has_value() ? SerializeMtlsIdentity(*identity) : nlohmann::json{{"role", "anonymous"}, {"tenant_id", "anonymous"}, {"node_id", "anonymous"}}},
  };
  for (const std::string field : {"registry_endpoint", "mirror_endpoint", "internal_control_plane_endpoint"}) {
    if (HasNonEmptyString(body, field)) {
      record[field] = body[field].get<std::string>();
    }
  }
  return record;
}

std::string
HexBytes(const unsigned char *data, const size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t index = 0; index < size; ++index) {
    out << std::setw(2) << static_cast<int>(data[index]);
  }
  return out.str();
}

std::string
Sha256Bytes(std::string_view payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::string
UtcTimestampPlusDays(const int days) {
  const auto now = std::chrono::system_clock::now() + std::chrono::hours(24 * days);
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string
UtcTimestampNow() {
  return UtcTimestampPlusDays(0);
}

int64_t
UnixSecondsNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch()
  )
    .count();
}

std::string
RequestSubject(const HttpRequest &request) {
  if (request.identity.has_value()) {
    return request.identity->node_id;
  }
  if (const auto token = RegistryTokenFromHeaders(request.headers); token.has_value()) {
    const std::string hash = Sha256Bytes(*token);
    return "token:" + hash.substr(0, 12);
  }
  return "anonymous";
}

}  // namespace

}  // namespace spio::platform
