#pragma once

#include "PlatformCore/Config.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pafio::platform
{

inline constexpr size_t kDefaultHttpRequestBodyLimitBytes = 1U * 1024U * 1024U;
inline constexpr size_t kPublishHttpRequestBodyLimitBytes = 96U * 1024U * 1024U;

struct BeastServerOptions
{
  bool once = false;
};

size_t HttpRequestBodyLimitForTarget(std::string_view target);
bool HttpRequestBodySizeAllowed(std::string_view target, uint64_t content_length);
std::optional<size_t> CheckedHttpRequestSize(size_t header_bytes, uint64_t content_length);
nlohmann::json DescribeBeastServerCapability();
int RunBeastServer(const PlatformConfig &config, BeastServerOptions options = {});

}  // namespace pafio::platform
