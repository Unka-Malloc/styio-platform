#include "PlatformCloud/PackageRegistry/ReleaseManagement/ReleaseManager.hpp"

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace pafio::platform
{

nlohmann::json BuildReleaseRolloutPlan(
    const std::string &channel,
    const std::string &publication_id,
    const int percentage,
    const std::string &ring,
    const std::string &updated_at)
{
  if (channel.empty())
  {
    throw std::runtime_error("channel is required");
  }
  if (publication_id.empty())
  {
    throw std::runtime_error("publication_id is required");
  }
  if (percentage < 0 || percentage > 100)
  {
    throw std::runtime_error("percentage must be between 0 and 100");
  }
  return {
      {"channel", channel},
      {"publication_id", publication_id},
      {"percentage", percentage},
      {"ring", ring.empty() ? "default" : ring},
      {"updated_at", updated_at},
  };
}

void WriteReleaseChannel(const fs::path &registry_root, const nlohmann::json &plan)
{
  const std::string channel = plan.at("channel").get<std::string>();
  fs::create_directories(registry_root / "_release_channels");
  std::ofstream out(registry_root / "_release_channels" / (channel + ".json"), std::ios::binary | std::ios::trunc);
  out << plan.dump(2) << "\n";
}

nlohmann::json ListReleaseChannels(const fs::path &registry_root)
{
  nlohmann::json channels = nlohmann::json::array();
  const fs::path root = registry_root / "_release_channels";
  std::error_code ec;
  if (!fs::exists(root, ec))
  {
    return channels;
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(root, ec))
  {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
    {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    channels.push_back(nlohmann::json::parse(in));
  }
  return channels;
}

}  // namespace pafio::platform
