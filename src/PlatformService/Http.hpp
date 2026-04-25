#pragma once

#include "PlatformService/Identity.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace spio::platform
{

enum class HttpMethod
{
  Get,
  Post,
};

struct HttpRequest
{
  HttpMethod method = HttpMethod::Get;
  std::string path;
  std::map<std::string, std::string> headers;
  nlohmann::json body;
  std::optional<MtlsIdentity> identity;
};

struct HttpResponse
{
  int status_code = 200;
  nlohmann::json body;
};

struct RouteSpec
{
  std::string operation_id;
  HttpMethod method = HttpMethod::Get;
  std::string path;
  bool internal = false;
};

struct RouteMatch
{
  RouteSpec route;
  std::map<std::string, std::string> parameters;
};

std::string ToString(HttpMethod method);
std::optional<HttpMethod> ParseHttpMethod(std::string_view method);
std::optional<RouteMatch> MatchRoute(const std::vector<RouteSpec> &routes, HttpMethod method, const std::string &path);
nlohmann::json SuccessEnvelope(std::string message, nlohmann::json payload);
nlohmann::json FailureEnvelope(std::string message, std::string detail, std::string category, std::string operation_id, int returncode = 17);

}  // namespace spio::platform
