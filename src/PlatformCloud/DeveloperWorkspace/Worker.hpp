#pragma once

#include "PlatformCore/Config.hpp"

namespace pafio::platform
{

struct WorkerOptions
{
  bool once = false;
};

int RunWorker(const PlatformConfig &config, WorkerOptions options = {});

}  // namespace pafio::platform
