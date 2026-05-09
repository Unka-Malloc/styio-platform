#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace spio::platform
{

// Normalized actor shape shared by Microsoft, Google, Apple, and Telegram
// identity assertions before platform authorization maps roles.
struct ExternalIdentityRecord
{
  std::string provider;
  std::string subject;
  std::string email;
  std::string tenant_id;
  std::string actor_id;
  nlohmann::json roles = nlohmann::json::array();
  nlohmann::json claims = nlohmann::json::object();
};

// Converts a provider-specific assertion into a platform actor. The function
// validates required identity claims instead of trusting provider payload shape.
ExternalIdentityRecord NormalizeExternalIdentity(const nlohmann::json &request);

nlohmann::json SerializeExternalIdentityRecord(const ExternalIdentityRecord &record);

}  // namespace spio::platform
