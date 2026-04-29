#pragma once

#include "PlatformService/Config.hpp"

namespace spio::platform
{

struct WorkerOptions
{
  bool once = false;
};

int RunWorker(const PlatformConfig &config, WorkerOptions options = {});

}  // namespace spio::platform
