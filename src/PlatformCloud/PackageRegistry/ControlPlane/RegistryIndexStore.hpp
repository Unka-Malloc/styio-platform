#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryIdentifiers.hpp"
#include "PlatformCore/Core/Sha256.hpp"

namespace spio::platform
{

namespace
{

// Canonical JSON is the digest input for index, publication, and trust
// metadata. Pretty-printed JSON is only used for files humans inspect.
std::string CanonicalJson(const nlohmann::json &payload);

bool
JsonLineHasRelease(const std::string &line, const std::string &package, const std::string &version) {
  try {
    const nlohmann::json entry = nlohmann::json::parse(line);
    return entry.value("package", "") == package && entry.value("version", "") == version;
  }
  catch (...) {
    return false;
  }
}

bool
ReleaseExistsOnDisk(const fs::path &registry_root, const std::string &package, const std::string &version) {
  const fs::path index_path = registry_root / RegistryIndexPathForPackage(package);
  std::ifstream in(index_path);
  if (!in) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (JsonLineHasRelease(line, package, version)) {
      return true;
    }
  }
  return false;
}

std::vector<nlohmann::json>
ReadPackageIndexRecords(const fs::path &registry_root, const std::string &package) {
  const fs::path index_path = registry_root / RegistryIndexPathForPackage(package);
  std::vector<nlohmann::json> records;
  std::ifstream in(index_path);
  if (!in) {
    return records;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

void
WritePackageIndexRecords(
  const fs::path &registry_root,
  const std::string &package,
  const std::vector<nlohmann::json> &records
) {
  const fs::path index_path = registry_root / RegistryIndexPathForPackage(package);
  fs::create_directories(index_path.parent_path());
  std::ofstream out(index_path, std::ios::binary | std::ios::trunc);
  for (const nlohmann::json &record : records) {
    out << record.dump() << "\n";
  }
}

nlohmann::json
PackagePayloadFromIndex(const fs::path &registry_root, const std::string &package) {
  const std::vector<nlohmann::json> records = ReadPackageIndexRecords(registry_root, package);
  if (records.empty()) {
    throw std::runtime_error("package is not found");
  }
  const size_t slash = package.find('/');
  std::vector<std::string> versions;
  nlohmann::json releases = nlohmann::json::array();
  for (const nlohmann::json &record : records) {
    versions.push_back(record.at("version").get<std::string>());
    releases.push_back({
      {"version", record.at("version").get<std::string>()},
      {"publisher_id", record.value("publisher_id", "")},
      {"published_at", record.value("published_at", "")},
      {"yanked", record.value("yanked", false)},
      {"source_artifact_sha256", record.at("source_artifact").at("sha256").get<std::string>()},
      {"source_artifact_path", record.at("source_artifact").at("path").get<std::string>()},
    });
  }
  std::sort(versions.begin(), versions.end(), VersionLess);
  return {
    {"package_id", package},
    {"namespace", package.substr(0, slash)},
    {"name", package.substr(slash + 1)},
    {"visibility", "public"},
    {"latest_version", versions.back()},
    {"release_count", static_cast<int64_t>(records.size())},
    {"releases", releases},
  };
}

std::optional<nlohmann::json>
PackageReleasePayloadFromIndex(
  const fs::path &registry_root,
  const std::string &package,
  const std::string &version
) {
  for (const nlohmann::json &record : ReadPackageIndexRecords(registry_root, package)) {
    if (record.value("version", "") == version) {
      return record;
    }
  }
  return std::nullopt;
}

size_t
CountRegularFiles(const fs::path &root) {
  std::error_code ec;
  if (!fs::exists(root, ec)) {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec)) {
    if (entry.is_regular_file(ec)) {
      ++count;
    }
  }
  return count;
}

size_t
CountIndexReleases(const fs::path &index_root) {
  std::error_code ec;
  if (!fs::exists(index_root, ec)) {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(index_root, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".jsonl") {
      continue;
    }
    std::ifstream in(entry.path());
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty()) {
        ++count;
      }
    }
  }
  return count;
}

size_t
CountNamespaces(const fs::path &index_root) {
  std::error_code ec;
  if (!fs::exists(index_root, ec)) {
    return 0;
  }
  size_t count = 0;
  for (const fs::directory_entry &entry : fs::directory_iterator(index_root, ec)) {
    if (entry.is_directory(ec)) {
      ++count;
    }
  }
  return count;
}

void
WriteJsonFileIfMissing(const fs::path &path, const nlohmann::json &payload) {
  if (fs::exists(path)) {
    return;
  }
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << payload.dump(2) << "\n";
}

void
AppendJsonLine(const fs::path &path, const nlohmann::json &payload) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::app);
  out << payload.dump() << "\n";
}

std::string
ReadFileBytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string
DirectoryDigest(const fs::path &root) {
  std::error_code ec;
  nlohmann::json files = nlohmann::json::array();
  if (!fs::exists(root, ec)) {
    return "sha256:" + Sha256Bytes(CanonicalJson(files));
  }
  std::vector<fs::path> paths;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root, ec)) {
    if (entry.is_regular_file(ec)) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const fs::path &path : paths) {
    files.push_back({
      {"path", fs::relative(path, root).generic_string()},
      {"sha256", spio::Sha256File(path)},
    });
  }
  return "sha256:" + Sha256Bytes(CanonicalJson(files));
}

size_t
NextPublicationSequenceOnDisk(const fs::path &registry_root) {
  const fs::path publications_root = registry_root / "_publications";
  std::error_code ec;
  size_t sequence = 1;
  if (!fs::exists(publications_root, ec)) {
    return sequence;
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(publications_root, ec)) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.starts_with("pub-")) {
      try {
        sequence = std::max(sequence, static_cast<size_t>(std::stoul(name.substr(4)) + 1U));
      }
      catch (...) {
      }
    }
  }
  return sequence;
}

void
CopyRegistryReadPlaneTo(const fs::path &registry_root, const fs::path &dest) {
  fs::create_directories(dest);
  std::error_code ec;
  for (const std::string entry : {"index", "artifacts", "trust", "log"}) {
    const fs::path source = registry_root / entry;
    if (fs::exists(source, ec)) {
      fs::copy(source, dest / entry, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
      if (ec) {
        throw std::runtime_error("failed to copy registry publication tree: " + ec.message());
      }
    }
  }
  fs::copy_file(registry_root / "config.json", dest / "config.json", fs::copy_options::overwrite_existing, ec);
  if (ec) {
    throw std::runtime_error("failed to copy registry publication config: " + ec.message());
  }
}

void
MaterializePublicationToRoot(const fs::path &registry_root, const fs::path &publication_root) {
  std::error_code ec;
  fs::remove(registry_root / "config.json", ec);
  for (const std::string entry : {"index", "artifacts", "trust", "log"}) {
    fs::remove_all(registry_root / entry, ec);
  }
  CopyRegistryReadPlaneTo(publication_root, registry_root);
}

std::optional<nlohmann::json>
CurrentDistributionPointer(const fs::path &registry_root, const std::string &distribution_id) {
  const fs::path pointer = registry_root / "_distributions" / distribution_id / "current.json";
  if (!fs::exists(pointer)) {
    return std::nullopt;
  }
  return nlohmann::json::parse(ReadFileBytes(pointer));
}

void
WriteTextFile(const fs::path &path, const std::string &payload) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << payload;
}

std::string
CanonicalJson(const nlohmann::json &payload) {
  return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

std::string
JsonText(const nlohmann::json &payload) {
  return payload.dump(2) + "\n";
}

}  // namespace

}  // namespace spio::platform
