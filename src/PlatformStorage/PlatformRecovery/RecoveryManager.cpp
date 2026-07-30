#include "PlatformStorage/PlatformRecovery/RecoveryManager.hpp"

#include "PlatformCore/Core/Sha256.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace pafio::platform
{

namespace
{

void WriteJsonFile(const fs::path &path, const nlohmann::json &payload)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << payload.dump(2) << "\n";
}

nlohmann::json ReadJsonFile(const fs::path &path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("failed to read snapshot manifest: " + path.string());
  }
  return nlohmann::json::parse(in);
}

nlohmann::json FileManifest(const fs::path &root)
{
  nlohmann::json files = nlohmann::json::array();
  std::error_code ec;
  if (!fs::exists(root, ec))
  {
    return files;
  }
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
        {"sha256", Sha256File(path)},
        {"size_bytes", static_cast<int64_t>(fs::file_size(path))},
    });
  }
  return files;
}

void CopyTree(const fs::path &source, const fs::path &target)
{
  std::error_code ec;
  fs::create_directories(target);
  if (!fs::exists(source, ec))
  {
    return;
  }
  fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec)
  {
    throw std::runtime_error("failed to copy snapshot tree: " + ec.message());
  }
}

}  // namespace

nlohmann::json CreateFilesystemSnapshot(const RecoverySnapshotRequest &request)
{
  if (request.snapshot_id.empty())
  {
    throw std::runtime_error("snapshot_id is required");
  }
  const fs::path snapshot_root = request.snapshots_root / request.snapshot_id;
  const fs::path data_root = snapshot_root / "data";
  std::error_code ec;
  if (fs::exists(snapshot_root, ec))
  {
    throw std::runtime_error("snapshot already exists: " + request.snapshot_id);
  }
  fs::create_directories(snapshot_root.parent_path());
  const fs::path temp_root = request.snapshots_root / "_tmp" / request.snapshot_id;
  fs::remove_all(temp_root, ec);
  CopyTree(request.source_root, temp_root / "data");

  nlohmann::json manifest = {
      {"snapshot_id", request.snapshot_id},
      {"label", request.label},
      {"created_at", request.created_at},
      {"source_root", request.source_root.generic_string()},
      {"file_count", static_cast<int64_t>(FileManifest(temp_root / "data").size())},
      {"files", FileManifest(temp_root / "data")},
  };
  WriteJsonFile(temp_root / "snapshot.json", manifest);
  fs::rename(temp_root, snapshot_root, ec);
  if (ec)
  {
    throw std::runtime_error("failed to activate snapshot: " + ec.message());
  }
  manifest["root_path"] = snapshot_root.generic_string();
  manifest["data_path"] = data_root.generic_string();
  return manifest;
}

nlohmann::json RestoreFilesystemSnapshot(
    const fs::path &snapshots_root,
    const std::string &snapshot_id,
    const fs::path &target_root,
    const std::string &restored_at)
{
  if (snapshot_id.empty())
  {
    throw std::runtime_error("snapshot_id is required");
  }
  const fs::path snapshot_root = snapshots_root / snapshot_id;
  const fs::path data_root = snapshot_root / "data";
  if (!fs::exists(snapshot_root / "snapshot.json") || !fs::exists(data_root))
  {
    throw std::runtime_error("snapshot is not found: " + snapshot_id);
  }
  nlohmann::json manifest = ReadJsonFile(snapshot_root / "snapshot.json");
  const nlohmann::json expected_files = manifest.value("files", nlohmann::json::array());
  const nlohmann::json actual_files = FileManifest(data_root);
  if (expected_files != actual_files)
  {
    throw std::runtime_error("snapshot verification failed before restore");
  }
  std::error_code ec;
  fs::remove_all(target_root, ec);
  CopyTree(data_root, target_root);
  return {
      {"snapshot_id", snapshot_id},
      {"restored_at", restored_at},
      {"target_root", target_root.generic_string()},
      {"file_count", static_cast<int64_t>(actual_files.size())},
      {"verified", true},
  };
}

nlohmann::json ListFilesystemSnapshots(const fs::path &snapshots_root)
{
  nlohmann::json snapshots = nlohmann::json::array();
  std::error_code ec;
  if (!fs::exists(snapshots_root, ec))
  {
    return snapshots;
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(snapshots_root, ec))
  {
    if (!entry.is_directory(ec) || entry.path().filename() == "_tmp")
    {
      continue;
    }
    const fs::path manifest_path = entry.path() / "snapshot.json";
    if (fs::exists(manifest_path, ec))
    {
      snapshots.push_back(ReadJsonFile(manifest_path));
    }
  }
  return snapshots;
}

nlohmann::json BuildStorageStatus(
    const std::string &state_backend,
    const std::string &object_store_provider,
    const fs::path &registry_root,
    const fs::path &snapshots_root)
{
  std::error_code ec;
  return {
      {"state_backend", state_backend},
      {"object_store_provider", object_store_provider},
      {"registry_root_present", fs::exists(registry_root, ec)},
      {"snapshots_root_present", fs::exists(snapshots_root, ec)},
      {"registry_root", "<redacted>"},
      {"snapshots_root", "<redacted>"},
  };
}

}  // namespace pafio::platform
