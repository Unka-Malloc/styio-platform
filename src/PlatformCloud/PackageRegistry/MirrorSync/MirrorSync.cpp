#include "PlatformCloud/PackageRegistry/MirrorSync/MirrorSync.hpp"

#include "PlatformCore/Core/Sha256.hpp"
#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace pafio::platform
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

std::string ReadTextFile(const fs::path &path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("failed to read " + path.string());
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void WriteJsonFile(const fs::path &path, const nlohmann::json &payload)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << payload.dump(2) << "\n";
}

size_t CountFiles(const fs::path &root)
{
  std::error_code ec;
  if (!fs::exists(root, ec))
  {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec))
  {
    if (entry.is_regular_file(ec))
    {
      ++count;
    }
  }
  return count;
}

std::string HexBytes(const unsigned char *data, const size_t size)
{
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t index = 0; index < size; ++index)
  {
    out << std::setw(2) << static_cast<int>(data[index]);
  }
  return out.str();
}

std::string Sha256Bytes(std::string_view payload)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(payload.data()), payload.size(), digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::string DirectoryDigest(const fs::path &root)
{
  nlohmann::json files = nlohmann::json::array();
  std::error_code ec;
  if (fs::exists(root, ec))
  {
    std::vector<fs::path> paths;
    for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec))
    {
      if (entry.is_regular_file(ec))
      {
        paths.push_back(entry.path());
      }
    }
    std::sort(paths.begin(), paths.end());
    for (const fs::path &path : paths)
    {
      files.push_back({
          {"path", fs::relative(path, root).generic_string()},
          {"sha256", pafio::Sha256File(path)},
      });
    }
  }
  return "sha256:" + Sha256Bytes(files.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict));
}

void VerifyPublicationSnapshot(const fs::path &publication_root, const nlohmann::json &publication)
{
  if (!fs::exists(publication_root / "config.json") || !fs::exists(publication_root / "trust" / "root.json") ||
      !fs::exists(publication_root / "publication.json"))
  {
    throw std::runtime_error("publication snapshot is incomplete");
  }
  if (publication.value("verified", false) != true)
  {
    throw std::runtime_error("origin publication is not verified");
  }
  if (publication.value("artifact_count", 0) != static_cast<int>(CountFiles(publication_root / "artifacts")))
  {
    throw std::runtime_error("publication artifact count mismatch");
  }
  if (publication.value("index_digest", "") != DirectoryDigest(publication_root / "index"))
  {
    throw std::runtime_error("publication index digest mismatch");
  }
  if (publication.value("trust_digest", "") != DirectoryDigest(publication_root / "trust"))
  {
    throw std::runtime_error("publication trust digest mismatch");
  }
  const size_t tree_size = CountLogLeaves(publication_root);
  if (publication.value("tree_size", 0) != static_cast<int>(tree_size))
  {
    throw std::runtime_error("publication tree size mismatch");
  }
}

void MaterializePublication(const fs::path &publication_root, const fs::path &target_root)
{
  std::error_code ec;
  fs::remove(target_root / "config.json", ec);
  for (const std::string &relative : {"trust", "artifacts", "index", "log"})
  {
    fs::remove_all(target_root / relative, ec);
  }
  for (const std::string &relative : {"config.json", "trust", "artifacts", "index", "log"})
  {
    CopyIfPresent(publication_root, target_root, relative);
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
    const fs::path origin_pointer = source_root / "_distributions" / "default" / "current.json";
    if (!fs::exists(origin_pointer))
    {
      throw std::runtime_error("origin distribution current pointer is missing");
    }
    const nlohmann::json current = nlohmann::json::parse(ReadTextFile(origin_pointer));
    const std::string publication_id = current.at("publication_id").get<std::string>();
    const fs::path origin_publication_root = source_root / "_publications" / publication_id;
    const fs::path origin_publication_path = origin_publication_root / "publication.json";
    if (!fs::exists(origin_publication_path))
    {
      throw std::runtime_error("origin publication manifest is missing");
    }
    const nlohmann::json publication = nlohmann::json::parse(ReadTextFile(origin_publication_path));

    fs::create_directories(target_root);
    const fs::path temp_publication_root = target_root / "_tmp" / "mirror" / publication_id;
    std::error_code ec;
    fs::remove_all(temp_publication_root, ec);
    fs::create_directories(temp_publication_root.parent_path());
    fs::copy(
        origin_publication_root,
        temp_publication_root,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing,
        ec);
    if (ec)
    {
      throw std::runtime_error("mirror publication copy failed: " + ec.message());
    }
    VerifyPublicationSnapshot(temp_publication_root, publication);

    const fs::path final_publication_root = target_root / "_publications" / publication_id;
    fs::create_directories(final_publication_root.parent_path());
    fs::remove_all(final_publication_root, ec);
    fs::rename(temp_publication_root, final_publication_root, ec);
    if (ec)
    {
      throw std::runtime_error("mirror publication activation failed: " + ec.message());
    }
    MaterializePublication(final_publication_root, target_root);
    WriteJsonFile(target_root / "_distributions" / "default" / "current.json", current);

    const size_t tree_size = CountLogLeaves(final_publication_root);
    const std::string replay_cursor = "checkpoint-" + PaddedNumber(tree_size, 4);
    if (config.state_backend == "postgres")
    {
      PostgresStore store(config.postgres_dsn);
      store.RecordMirrorState(
          config.registry.mirror_id,
          config.region,
          config.registry.mirror_origin,
          "fresh",
          replay_cursor,
          publication_id,
          publication.value("repository_version_id", std::string()),
          current.value("updated_at", std::string()),
          static_cast<int>(tree_size));
    }
    std::cout << "mirror sync completed: " << publication_id << " " << replay_cursor << "\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << "mirror sync failed: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace pafio::platform
