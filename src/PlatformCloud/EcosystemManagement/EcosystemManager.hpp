#pragma once

#include <nlohmann/json.hpp>

namespace spio::platform
{

nlohmann::json DefaultEcosystemRepositories();
nlohmann::json BuildEcosystemReleasePlan(const nlohmann::json &request);

}  // namespace spio::platform
