#pragma once

#include "PlatformCore/Config.hpp"
#include "PlatformStorage/PlatformPersistence/StateRecords.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace spio::platform
{

std::optional<std::string> ValidateSubmitJobRequest(const nlohmann::json &request);
PlatformJobRecord BuildQueuedJobRecord(const nlohmann::json &request, const PlatformConfig &config, std::string job_id);

}  // namespace spio::platform
