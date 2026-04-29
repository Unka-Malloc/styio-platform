#pragma once

#include "PlatformService/Config.hpp"

#include <nlohmann/json.hpp>

namespace spio::platform
{

struct BeastServerOptions
{
  bool once = false;
};

nlohmann::json DescribeBeastServerCapability();
int RunBeastServer(const PlatformConfig &config, BeastServerOptions options = {});

}  // namespace spio::platform
