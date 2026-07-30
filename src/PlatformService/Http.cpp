#include "PlatformService/Http.hpp"

#include <sstream>

namespace pafio::platform
{

namespace
{

std::vector<std::string> SplitRoute(std::string_view route)
{
  std::vector<std::string> parts;
  std::stringstream stream{std::string(route)};
  std::string item;
  while (std::getline(stream, item, '/'))
  {
    if (!item.empty())
    {
      parts.push_back(item);
    }
  }
  return parts;
}

bool IsParameter(std::string_view segment)
{
  return segment.size() > 2 && segment.front() == '{' && segment.back() == '}';
}

std::string ParameterName(std::string_view segment)
{
  return std::string(segment.substr(1, segment.size() - 2));
}

}  // namespace

std::string ToString(HttpMethod method)
{
  switch (method)
  {
    case HttpMethod::Get:
      return "GET";
    case HttpMethod::Post:
      return "POST";
    case HttpMethod::Delete:
      return "DELETE";
  }
  return "GET";
}

std::optional<HttpMethod> ParseHttpMethod(std::string_view method)
{
  if (method == "GET")
  {
    return HttpMethod::Get;
  }
  if (method == "POST")
  {
    return HttpMethod::Post;
  }
  if (method == "DELETE")
  {
    return HttpMethod::Delete;
  }
  return std::nullopt;
}

std::optional<RouteMatch> MatchRoute(const std::vector<RouteSpec> &routes, HttpMethod method, const std::string &path)
{
  const std::vector<std::string> actual_parts = SplitRoute(path);
  for (const RouteSpec &route : routes)
  {
    if (route.method != method)
    {
      continue;
    }
    const std::vector<std::string> route_parts = SplitRoute(route.path);
    if (route_parts.size() != actual_parts.size())
    {
      continue;
    }
    std::map<std::string, std::string> parameters;
    bool matched = true;
    for (size_t index = 0; index < route_parts.size(); ++index)
    {
      if (IsParameter(route_parts[index]))
      {
        parameters[ParameterName(route_parts[index])] = actual_parts[index];
        continue;
      }
      if (route_parts[index] != actual_parts[index])
      {
        matched = false;
        break;
      }
    }
    if (matched)
    {
      return RouteMatch{.route = route, .parameters = parameters};
    }
  }
  return std::nullopt;
}

nlohmann::json SuccessEnvelope(std::string message, nlohmann::json payload)
{
  return {
      {"returncode", 0},
      {"message", std::move(message)},
      {"stdout", ""},
      {"stderr", ""},
      {"payload", std::move(payload)},
  };
}

nlohmann::json FailureEnvelope(std::string message, std::string detail, std::string category, std::string operation_id, int returncode)
{
  return {
      {"returncode", returncode},
      {"message", std::move(message)},
      {"stdout", ""},
      {"stderr", detail},
      {"error_payload", {
                            {"category", std::move(category)},
                            {"detail", std::move(detail)},
                            {"operation_id", std::move(operation_id)},
                        }},
  };
}

}  // namespace pafio::platform
