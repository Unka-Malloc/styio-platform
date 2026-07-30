#pragma once

#include "PlatformService/Http.hpp"

#include <vector>

namespace pafio::platform
{

std::vector<RouteSpec> BuildRegistryControlPlaneRoutes();

}  // namespace pafio::platform
