#pragma once

#include "SpioPlatformProtocols/Execution.hpp"
#include "PlatformCore/Toolchain/State.hpp"

#include <nlohmann/json.hpp>

namespace spio
{

nlohmann::json SerializeSharedCachePolicy(const SharedCachePolicy &policy);
nlohmann::json SerializeWorkerPoolKey(const WorkerPoolKey &key);
nlohmann::json SerializeCloudExecutionPolicy(const CloudExecutionPolicy &policy);
nlohmann::json SerializeProjectToolchainState(const ProjectToolchainState &state);
nlohmann::json BuildCloudStatusPayload(const ProjectToolchainState &state, const CloudExecutionPolicy &policy);

}  // namespace spio
