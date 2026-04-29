#include "PlatformService/MirrorSync.hpp"

#include "PlatformService/PostgresStore.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

std::string PaddedNumber(const size_t value, const int width)
{
  std::ostringstream stream;
  stream.width(width);
  stream.fill('0');
  stream << value;
  return stream.str();
}

size_t CountLogLeaves(const fs::path &root)
{
  std::error_code ec;
  const fs::path leaves = root / "log" / "leaves";
  if (!fs::exists(leaves, ec))
  {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(leaves, ec))
  {
    if (entry.is_regular_file(ec))
    {
      ++count;
    }
  }
  return count;
}

void CopyIfPresent(const fs::path &source_root, const fs::path &target_root, const std::string &relative)
{
  std::error_code ec;
  const fs::path source = source_root / relative;
  if (!fs::exists(source, ec))
  {
    return;
  }
  const fs::path target = target_root / relative;
  fs::create_directories(target.parent_path(), ec);
  fs::copy(
      source,
      target,
      fs::copy_options::recursive | fs::copy_options::overwrite_existing,
      ec);
  if (ec)
  {
    throw std::runtime_error("mirror copy failed for " + relative + ": " + ec.message());
  }
}

}  // namespace

int RunMirrorSyncOnce(const PlatformConfig &config)
{
  try
  {
    if (config.registry.mirror_source_root.empty())
    {
      throw std::runtime_error("STYIO_PLATFORM_REGISTRY_MIRROR_SOURCE_ROOT is required");
    }
    const fs::path source_root(config.registry.mirror_source_root);
    const fs::path target_root(config.registry.root);
    if (!fs::exists(source_root) || !fs::is_directory(source_root))
    {
      throw std::runtime_error("mirror source root does not exist or is not a directory");
    }
    fs::create_directories(target_root);

    for (const std::string &relative : std::vector<std::string>{"config.json", "trust", "artifacts", "index", "log"})
    {
      CopyIfPresent(source_root, target_root, relative);
    }

    const size_t tree_size = CountLogLeaves(target_root);
    const std::string replay_cursor = "checkpoint-" + PaddedNumber(tree_size, 4);
    if (config.state_backend == "postgres")
    {
      PostgresStore store(config.postgres_dsn);
      store.RecordMirrorState(
          config.registry.mirror_id,
          config.region,
          config.registry.mirror_origin,
          "fresh",
          replay_cursor);
    }
    std::cout << "mirror sync completed: " << replay_cursor << "\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "mirror sync failed: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace spio::platform
