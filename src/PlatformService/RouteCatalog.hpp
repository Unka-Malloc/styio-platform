#pragma once

#include <string_view>
#include <vector>

#include "PlatformService/Http.hpp"

namespace pafio::platform
{

// Returns the platform control-plane routes owned by PlatformService. Registry
// routes are built separately by PackageRegistry to keep extraction boundaries
// visible.
std::vector<RouteSpec> BuildPlatformControlPlaneRoutes();

// Classifies operations whose error envelopes must preserve registry
// control-plane compatibility.
bool IsRegistryOperation(std::string_view operation_id);

// Returns true for registry mutations that can be authorized by publish tokens
// instead of only by an mTLS platform identity.
bool IsRegistryTokenCapableOperation(std::string_view operation_id);

}  // namespace pafio::platform
