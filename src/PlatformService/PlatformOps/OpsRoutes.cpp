#include "PlatformSecurity/ExternalIdentity/ExternalIdentity.hpp"
#include "PlatformService/RouterSupport.hpp"
#include "PlatformStorage/PlatformRecovery/RecoveryManager.hpp"

namespace pafio::platform
{

HttpResponse
PlatformRouter::HandleCreateRecoverySnapshot(const HttpRequest &request) {
  if (!request.body.is_object()) {
    return FailureResponse(400, "recovery snapshot rejected", "request body must be an object", "ValidationError", "createRecoverySnapshot", 2);
  }
  const fs::path registry_root(config_.registry.root);
  const fs::path snapshots_root = registry_root.parent_path() / "recovery-snapshots";
  const std::string snapshot_id =
    request.body.value("snapshot_id", "snap-" + PaddedNumber(ListFilesystemSnapshots(snapshots_root).size() + 1, 6));
  try {
    const nlohmann::json snapshot = CreateFilesystemSnapshot({
      .source_root = registry_root,
      .snapshots_root = snapshots_root,
      .snapshot_id = snapshot_id,
      .label = request.body.value("label", "manual"),
      .created_at = UtcTimestampNow(),
    });
    RecordRegistryAudit(request, "createRecoverySnapshot", {{"snapshot_id", snapshot_id}}, "success");
    return JsonResponse(200, SuccessEnvelope("created recovery snapshot", snapshot));
  }
  catch (const std::exception &error) {
    RecordRegistryAudit(request, "createRecoverySnapshot", {{"snapshot_id", snapshot_id}}, "failed");
    return FailureResponse(422, "recovery snapshot failed", error.what(), "RecoveryError", "createRecoverySnapshot");
  }
}

HttpResponse
PlatformRouter::HandleListRecoverySnapshots() const {
  const fs::path snapshots_root = fs::path(config_.registry.root).parent_path() / "recovery-snapshots";
  return JsonResponse(
    200,
    SuccessEnvelope("loaded recovery snapshots", {{"snapshots", ListFilesystemSnapshots(snapshots_root)}})
  );
}

HttpResponse
PlatformRouter::HandleRestoreRecoverySnapshot(const RouteMatch &match, const HttpRequest &request) {
  const std::string snapshot_id = match.parameters.at("snapshot_id");
  try {
    const nlohmann::json restored = RestoreFilesystemSnapshot(
      fs::path(config_.registry.root).parent_path() / "recovery-snapshots",
      snapshot_id,
      fs::path(config_.registry.root),
      UtcTimestampNow()
    );
    RecordRegistryAudit(request, "restoreRecoverySnapshot", {{"snapshot_id", snapshot_id}}, "success");
    return JsonResponse(200, SuccessEnvelope("restored recovery snapshot", restored));
  }
  catch (const std::exception &error) {
    RecordRegistryAudit(request, "restoreRecoverySnapshot", {{"snapshot_id", snapshot_id}}, "failed");
    return FailureResponse(422, "recovery restore failed", error.what(), "RecoveryError", "restoreRecoverySnapshot");
  }
}

HttpResponse
PlatformRouter::HandleListAuditEvents() const {
  return JsonResponse(
    200,
    SuccessEnvelope(
      "loaded audit events",
      {{"events", postgres_ != nullptr ? postgres_->ListRegistryAuditEvents() : memory_.ListRegistryAuditEvents()}}
    )
  );
}

HttpResponse
PlatformRouter::HandleMetrics() const {
  return JsonResponse(
    200,
    SuccessEnvelope("loaded platform metrics", {{"requests", metrics_.Snapshot()}, {"rate_limiter", rate_limiter_.Snapshot()}})
  );
}

HttpResponse
PlatformRouter::HandleStorageStatus() const {
  return JsonResponse(
    200,
    SuccessEnvelope(
      "loaded storage status",
      BuildStorageStatus(
        config_.state_backend,
        config_.object_store.provider,
        fs::path(config_.registry.root),
        fs::path(config_.registry.root).parent_path() / "recovery-snapshots"
      )
    )
  );
}

HttpResponse
PlatformRouter::HandleExchangeExternalIdentity(const HttpRequest &request) const {
  try {
    return JsonResponse(
      200,
      SuccessEnvelope("exchanged external identity", SerializeExternalIdentityRecord(NormalizeExternalIdentity(request.body)))
    );
  }
  catch (const std::exception &error) {
    return FailureResponse(400, "external identity exchange failed", error.what(), "ExternalIdentityError", "exchangeExternalIdentity", 2);
  }
}

}  // namespace pafio::platform
