#pragma once

#include <optional>
#include <string>

#include "PlatformService/RouterSupport.hpp"

namespace pafio::platform
{

namespace
{

std::optional<std::string>
ValidateCompileContainerRegistration(const nlohmann::json &body) {
  if (!body.is_object()) {
    return "request body must be an object";
  }
  for (const std::string field : {"container_id", "worker_id", "tenant_id", "user_id", "workspace_id", "region", "worker_pool_key"}) {
    if (!HasNonEmptyString(body, field)) {
      return field + " is required";
    }
  }
  const int capacity = body.value("capacity", 0);
  if (capacity < 1) {
    return "capacity must be positive";
  }
  if (body.contains("status") && body["status"] != "active" && body["status"] != "draining") {
    return "status must be active or draining when present";
  }
  return std::nullopt;
}

std::optional<std::string>
ValidateCompileContainerSwitch(const nlohmann::json &body) {
  if (!body.is_object()) {
    return "request body must be an object";
  }
  for (const std::string field : {"worker_id", "workspace_id"}) {
    if (!HasNonEmptyString(body, field)) {
      return field + " is required";
    }
  }
  if (const std::optional<std::string> error = ValidateOptionalStringFields(body, {"tenant_id", "user_id", "reason"}); error.has_value()) {
    return error;
  }
  return std::nullopt;
}

}  // namespace

}  // namespace pafio::platform
