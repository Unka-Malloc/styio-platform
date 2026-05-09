#pragma once

#include <nlohmann/json.hpp>

namespace spio::platform
{

nlohmann::json DefaultDocumentationGovernance();
nlohmann::json BuildDocumentationChangePlan(const nlohmann::json &request);

}  // namespace spio::platform
