#pragma once

#include <nlohmann/json.hpp>

namespace pafio::platform
{

nlohmann::json DefaultDocumentationGovernance();
nlohmann::json BuildDocumentationChangePlan(const nlohmann::json &request);

}  // namespace pafio::platform
