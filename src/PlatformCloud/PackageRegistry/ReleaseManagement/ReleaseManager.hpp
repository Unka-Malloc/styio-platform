#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace spio::platform
{

nlohmann::json BuildReleaseRolloutPlan(
    const std::string &channel,
    const std::string &publication_id,
    int percentage,
    const std::string &ring,
    const std::string &updated_at);
void WriteReleaseChannel(const std::filesystem::path &registry_root, const nlohmann::json &plan);
nlohmann::json ListReleaseChannels(const std::filesystem::path &registry_root);

}  // namespace spio::platform

