#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace pafio::platform
{

// Filesystem snapshot request for registry and read-plane recovery. The caller
// supplies the timestamp so tests and control-plane audit records stay
// deterministic.
struct RecoverySnapshotRequest
{
  std::filesystem::path source_root;
  std::filesystem::path snapshots_root;
  std::string snapshot_id;
  std::string label;
  std::string created_at;
};

// Copies a registry tree into an immutable snapshot directory and records the
// file manifest used later by restore verification.
nlohmann::json CreateFilesystemSnapshot(const RecoverySnapshotRequest &request);

// Replaces target_root with a verified snapshot. The operation keeps recovery
// logic independent from the package registry control plane.
nlohmann::json RestoreFilesystemSnapshot(
  const std::filesystem::path &snapshots_root,
  const std::string &snapshot_id,
  const std::filesystem::path &target_root,
  const std::string &restored_at
);

nlohmann::json ListFilesystemSnapshots(const std::filesystem::path &snapshots_root);

// Summarizes the local storage surfaces exposed by the production ops API.
nlohmann::json BuildStorageStatus(
  const std::string &state_backend,
  const std::string &object_store_provider,
  const std::filesystem::path &registry_root,
  const std::filesystem::path &snapshots_root
);

}  // namespace pafio::platform
