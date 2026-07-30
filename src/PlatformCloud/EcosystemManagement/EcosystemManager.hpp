#pragma once

#include <nlohmann/json.hpp>

namespace pafio::platform
{

nlohmann::json DefaultEcosystemRepositories();
nlohmann::json BuildEcosystemReleasePlan(const nlohmann::json &request);

}  // namespace pafio::platform
