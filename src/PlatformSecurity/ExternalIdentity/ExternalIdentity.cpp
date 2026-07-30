#include "PlatformSecurity/ExternalIdentity/ExternalIdentity.hpp"

#include <array>
#include <stdexcept>

namespace pafio::platform
{

namespace
{

bool SupportedProvider(const std::string &provider)
{
  static constexpr std::array<std::string_view, 4> kProviders = {"microsoft", "google", "apple", "telegram"};
  for (const std::string_view candidate : kProviders)
  {
    if (provider == candidate)
    {
      return true;
    }
  }
  return false;
}

std::string RequireString(const nlohmann::json &payload, const std::string &field)
{
  if (!payload.contains(field) || !payload.at(field).is_string() || payload.at(field).get<std::string>().empty())
  {
    throw std::runtime_error(field + " is required");
  }
  return payload.at(field).get<std::string>();
}

}  // namespace

ExternalIdentityRecord NormalizeExternalIdentity(const nlohmann::json &request)
{
  if (!request.is_object())
  {
    throw std::runtime_error("request body must be an object");
  }
  const std::string provider = RequireString(request, "provider");
  if (!SupportedProvider(provider))
  {
    throw std::runtime_error("unsupported external identity provider");
  }
  const nlohmann::json claims = request.value("claims", nlohmann::json::object());
  if (!claims.is_object())
  {
    throw std::runtime_error("claims must be an object");
  }
  std::string subject;
  std::string email;
  if (provider == "telegram")
  {
    if (claims.contains("telegram_id"))
    {
      subject = claims.at("telegram_id").get<std::string>();
    }
    else if (claims.contains("id"))
    {
      subject = std::to_string(claims.at("id").get<int64_t>());
    }
    email = claims.value("username", "");
  }
  else
  {
    subject = claims.value("sub", "");
    email = claims.value("email", "");
  }
  if (subject.empty())
  {
    throw std::runtime_error("external subject is required");
  }
  const std::string tenant_id = request.value("tenant_id", provider);
  nlohmann::json roles = request.value("roles", nlohmann::json::array());
  if (!roles.is_array())
  {
    throw std::runtime_error("roles must be an array");
  }
  return {
      .provider = provider,
      .subject = subject,
      .email = email,
      .tenant_id = tenant_id,
      .actor_id = "external:" + provider + ":" + subject,
      .roles = roles,
      .claims = claims,
  };
}

nlohmann::json SerializeExternalIdentityRecord(const ExternalIdentityRecord &record)
{
  return {
      {"provider", record.provider},
      {"subject", record.subject},
      {"email", record.email},
      {"tenant_id", record.tenant_id},
      {"actor_id", record.actor_id},
      {"roles", record.roles},
      {"claims", record.claims},
      {"verification_mode", "claims-normalized"},
  };
}

}  // namespace pafio::platform

