#pragma once

#include "PlatformService/Http.hpp"

#include <vector>

namespace spio::platform
{

std::vector<RouteSpec> BuildRegistryControlPlaneRoutes();

}  // namespace spio::platform
