#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryControlPlaneSupport.hpp"

namespace spio::platform
{

HttpResponse
PlatformRouter::HandlePublishRelease(const HttpRequest &request) {
  if (!request.body.is_object()) {
    return FailureResponse(
      400,
      "malformed registry publish request",
      "request body must be a JSON object",
      "UsageError",
      "publishRelease",
      2
    );
  }
  if (const std::optional<std::string> error = ValidateOptionalStringFields(
        request.body,
        {"archive_path", "manifest_path", "package", "output_path", "publisher_id", "version"}
      );
      error.has_value()) {
    return FailureResponse(400, "malformed registry publish request", *error, "UsageError", "publishRelease", 2);
  }

  PublishDraft draft;
  try {
    draft = BuildPublishDraft(request.body, config_, request.identity);
  }
  catch (const std::exception &error) {
    return FailureResponse(
      400,
      "malformed registry publish request",
      error.what(),
      "UsageError",
      "publishRelease",
      2
    );
  }
  if (!RegistryWriteAuthorized(request, "package:publish", RegistryPackageId(draft.package))) {
    RecordRegistryAudit(
      request,
      "publishRelease",
      {{"package_id", draft.package}, {"version", draft.version}},
      "denied"
    );
    return FailureResponse(403, "registry publish denied", "token or identity lacks package:publish", "AuthError", "publishRelease", 2);
  }

  try {
    const fs::path registry_root(config_.registry.root);
    const std::string release_key = RegistryReleaseKey(draft.package, draft.version);
    if (UsesS3ObjectStore(config_)) {
      const bool remote_initialized = ObjectExists(config_.object_store, "config.json") || ObjectExists(config_.object_store, "trust/root.json");
      if (remote_initialized) {
        SyncS3RegistryStateToLocal(config_);
      }
      else {
        RemoveLocalRegistryMetadataCache(config_);
      }
    }

    const bool release_exists_in_postgres =
      postgres_ != nullptr && postgres_->GetRegistryPackageRelease(RegistryPackageId(draft.package), draft.version).has_value();
    if (memory_.HasPublishedRelease(release_key) || release_exists_in_postgres || ReleaseExistsOnDisk(registry_root, draft.package, draft.version)) {
      RecordRegistryAudit(
        request,
        "publishRelease",
        {{"package_id", draft.package}, {"version", draft.version}},
        "duplicate"
      );
      return FailureResponse(
        409,
        "registry publish failed",
        "package version is already published",
        "PublishError",
        "publishRelease"
      );
    }

    const bool created_root =
      !fs::exists(registry_root / "config.json") || !fs::exists(registry_root / "trust" / "root.json");
    EnsureRegistryRootInitialized(config_);
    const std::map<std::string, RegistryRoleKey> role_keys = LoadOrCreateRegistryRoleKeys(fs::path(config_.registry.key_dir));

    const std::string archive_sha256 = spio::Sha256File(draft.archive_path);
    const uintmax_t archive_size = fs::file_size(draft.archive_path);
    const std::string artifact_path =
      "artifacts/source/sha256/" + archive_sha256.substr(0, 2) + "/" + archive_sha256.substr(2, 2) + "/" + archive_sha256 + ".spio.src.tar";
    const std::string published_at = UtcTimestampNow();
    const nlohmann::json release_record =
      BuildReleaseRecord(draft, published_at, archive_sha256, archive_size, artifact_path);
    const LocalAppendResult append_result =
      AppendRegistryReleaseToLocal(config_, draft, release_record, artifact_path);
    const MetadataVersions metadata_versions = RefreshSignedRegistryMetadata(config_, role_keys, published_at);
    const nlohmann::json publication =
      CreateRegistryPublication("publish", RegistryReleaseKey(draft.package, draft.version), published_at);
    if (UsesS3ObjectStore(config_)) {
      UploadRegistryTreeToS3(config_);
    }

    nlohmann::json payload = {
      {"registry_root", registry_root.string()},
      {"registry_read_root", RegistryReadRootUrl(config_)},
      {"object_store_provider", config_.object_store.provider},
      {"created_root", created_root},
      {"package", draft.package},
      {"version", draft.version},
      {"publisher_id", draft.publisher_id},
      {"published_at", published_at},
      {"archive_path", draft.archive_path.string()},
      {"archive_sha256", archive_sha256},
      {"archive_size_bytes", static_cast<int64_t>(archive_size)},
      {"artifact_path", artifact_path},
      {"index_path", append_result.index_path},
      {"log_leaf_path", append_result.log_leaf_path},
      {"sequence", static_cast<int64_t>(append_result.sequence)},
      {"dependencies", draft.dependencies},
      {"dev_dependencies", draft.dev_dependencies},
      {"checkpoint_version", metadata_versions.checkpoint_version},
      {"snapshot_version", metadata_versions.snapshot_version},
      {"timestamp_version", metadata_versions.timestamp_version},
      {"namespaces", static_cast<int64_t>(metadata_versions.namespaces)},
      {"repository_id", "default"},
      {"repository_version_id", publication.at("repository_version_id").get<std::string>()},
      {"publication_id", publication.at("publication_id").get<std::string>()},
      {"distribution_id", "default"},
    };
    memory_.RecordPublishedRelease(release_key, payload);
    const size_t slash = draft.package.find('/');
    RegistryPackageRecord package_record{
      .package_id = RegistryPackageId(draft.package),
      .package_namespace = draft.package.substr(0, slash),
      .name = draft.package.substr(slash + 1),
      .created_at = published_at,
      .created_by = draft.publisher_id,
      .visibility = "public",
    };
    RegistryPackageReleaseRecord release_record_state{
      .package_id = RegistryPackageId(draft.package),
      .version = draft.version,
      .edition = "2026",
      .manifest_sha256 = release_record.at("manifest_digest").get<std::string>(),
      .source_artifact_sha256 = archive_sha256,
      .dependencies = release_record.at("dependencies"),
      .publisher_id = draft.publisher_id,
      .published_at = published_at,
      .yanked = false,
      .yanked_reason = "",
    };
    RegistryPackageOwnerRecord owner_record{
      .package_id = RegistryPackageId(draft.package),
      .owner_id = draft.publisher_id,
      .owner_kind = "user",
      .role = "owner",
      .added_by = RegistryActorId(request),
      .added_at = published_at,
    };
    if (postgres_ != nullptr) {
      postgres_->UpsertRegistryPackage(package_record);
      postgres_->UpsertRegistryPackageRelease(release_record_state);
      postgres_->AddRegistryPackageOwner(owner_record);
    }
    memory_.UpsertRegistryPackage(package_record);
    memory_.UpsertRegistryPackageRelease(release_record_state);
    memory_.AddRegistryPackageOwner(owner_record);
    RecordRegistryAudit(
      request,
      "publishRelease",
      {{"package_id", draft.package}, {"version", draft.version}, {"publication_id", publication.at("publication_id")}},
      "success"
    );
    return JsonResponse(200, SuccessEnvelope("published registry v2 release", payload));
  }
  catch (const std::exception &error) {
    RecordRegistryAudit(
      request,
      "publishRelease",
      {{"package_id", draft.package}, {"version", draft.version}},
      "failed"
    );
    return FailureResponse(422, "registry publish failed", error.what(), "PublishError", "publishRelease");
  }
}

HttpResponse
PlatformRouter::HandleVerifyRegistry(const HttpRequest &request) {
  if (!request.body.is_object() || !request.body.empty()) {
    return FailureResponse(
      400,
      "registry verification failed",
      "verify request must be an empty JSON object",
      "VerifyError",
      "verifyRegistry",
      2
    );
  }

  try {
    const fs::path registry_root(config_.registry.root);
    if (UsesS3ObjectStore(config_)) {
      if (!ObjectExists(config_.object_store, "config.json") || !ObjectExists(config_.object_store, "trust/root.json")) {
        return FailureResponse(
          422,
          "registry verification failed",
          "registry root is not initialized",
          "VerifyError",
          "verifyRegistry"
        );
      }
      SyncS3RegistryStateToLocal(config_);
    }
    if (!fs::exists(registry_root / "config.json") || !fs::exists(registry_root / "trust" / "root.json")) {
      return FailureResponse(
        422,
        "registry verification failed",
        "registry root is not initialized",
        "VerifyError",
        "verifyRegistry"
      );
    }
    const size_t namespaces = CountNamespaces(registry_root / "index");
    const size_t index_files = CountRegularFiles(registry_root / "index");
    const size_t releases = CountIndexReleases(registry_root / "index");
    const size_t tree_size = CountRegularFiles(registry_root / "log" / "leaves");
    nlohmann::json payload = {
      {"ok", true},
      {"root", registry_root.string()},
      {"registry_read_root", RegistryReadRootUrl(config_)},
      {"object_store_provider", config_.object_store.provider},
      {"namespaces", static_cast<int64_t>(namespaces)},
      {"index_files", static_cast<int64_t>(index_files)},
      {"releases", static_cast<int64_t>(releases)},
      {"tree_size", static_cast<int64_t>(tree_size)},
    };
    const std::optional<nlohmann::json> current = CurrentDistributionPointer(registry_root, "default");
    RecordMirrorState(
      "fresh",
      "checkpoint-" + PaddedNumber(tree_size, 4),
      current.has_value() ? current->value("publication_id", "") : "",
      current.has_value() ? current->value("repository_version_id", "") : "",
      UtcTimestampNow(),
      static_cast<int>(tree_size)
    );
    return JsonResponse(200, SuccessEnvelope("verified registry v2 root", payload));
  }
  catch (const std::exception &error) {
    return FailureResponse(422, "registry verification failed", error.what(), "VerifyError", "verifyRegistry");
  }
}

HttpResponse
PlatformRouter::HandleGetPackage(const RouteMatch &match) const {
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
    return FailureResponse(400, "package lookup rejected", *error, "ValidationError", "getPackage", 2);
  }
  try {
    if (postgres_ != nullptr) {
      const std::optional<RegistryPackageRecord> record = postgres_->GetRegistryPackage(RegistryPackageId(package));
      if (record.has_value()) {
        nlohmann::json payload = SerializeRegistryPackageRecord(*record);
        payload["releases"] = postgres_->ListRegistryPackageReleases(RegistryPackageId(package));
        payload["release_count"] = payload.at("releases").size();
        return JsonResponse(200, SuccessEnvelope("loaded package", payload));
      }
    }
    return JsonResponse(
      200,
      SuccessEnvelope(
        "loaded package",
        PackagePayloadFromIndex(fs::path(config_.registry.root), package)
      )
    );
  }
  catch (const std::exception &error) {
    return FailureResponse(404, "package lookup failed", error.what(), "NotFound", "getPackage");
  }
}

HttpResponse
PlatformRouter::HandleListPackageReleases(const RouteMatch &match) const {
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
    return FailureResponse(400, "package release lookup rejected", *error, "ValidationError", "listPackageReleases", 2);
  }
  const fs::path root(config_.registry.root);
  const std::vector<nlohmann::json> releases = ReadPackageIndexRecords(root, package);
  if (releases.empty()) {
    if (postgres_ != nullptr) {
      nlohmann::json persisted_releases = postgres_->ListRegistryPackageReleases(RegistryPackageId(package));
      if (!persisted_releases.empty()) {
        return JsonResponse(
          200,
          SuccessEnvelope(
            "loaded package releases",
            {{"package_id", package}, {"releases", persisted_releases}}
          )
        );
      }
    }
    return FailureResponse(404, "package release lookup failed", "package is not found", "NotFound", "listPackageReleases");
  }
  return JsonResponse(
    200,
    SuccessEnvelope(
      "loaded package releases",
      {{"package_id", package}, {"releases", releases}}
    )
  );
}

HttpResponse
PlatformRouter::HandleGetPackageRelease(const RouteMatch &match) const {
  const std::string package = RegistryPackageFromRoute(match);
  const std::string version = match.parameters.at("version");
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
    return FailureResponse(400, "package release lookup rejected", *error, "ValidationError", "getPackageRelease", 2);
  }
  const std::optional<nlohmann::json> release =
    PackageReleasePayloadFromIndex(fs::path(config_.registry.root), package, version);
  if (!release.has_value()) {
    if (postgres_ != nullptr) {
      const std::optional<RegistryPackageReleaseRecord> persisted_release =
        postgres_->GetRegistryPackageRelease(RegistryPackageId(package), version);
      if (persisted_release.has_value()) {
        return JsonResponse(
          200,
          SuccessEnvelope("loaded package release", SerializeRegistryPackageReleaseRecord(*persisted_release))
        );
      }
    }
    return FailureResponse(404, "package release lookup failed", "package release is not found", "NotFound", "getPackageRelease");
  }
  return JsonResponse(200, SuccessEnvelope("loaded package release", *release));
}

HttpResponse
PlatformRouter::HandleSetPackageReleaseYanked(
  const RouteMatch &match,
  const HttpRequest &request,
  const bool yanked
) {
  const std::string package = RegistryPackageFromRoute(match);
  const std::string version = match.parameters.at("version");
  const std::string operation = yanked ? "yankPackageRelease" : "unyankPackageRelease";
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
    return FailureResponse(400, "package release mutation rejected", *error, "ValidationError", operation, 2);
  }
  if (!request.body.is_object()) {
    return FailureResponse(400, "package release mutation rejected", "request body must be an object", "ValidationError", operation, 2);
  }
  if (request.body.contains("reason") && !request.body["reason"].is_string()) {
    return FailureResponse(400, "package release mutation rejected", "reason must be a string when present", "ValidationError", operation, 2);
  }
  if (!RegistryWriteAuthorized(request, yanked ? "package:yank" : "package:yank", RegistryPackageId(package))) {
    RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "denied");
    return FailureResponse(403, "package release mutation denied", "token or identity lacks package:yank", "AuthError", operation, 2);
  }

  try {
    const fs::path root(config_.registry.root);
    std::vector<nlohmann::json> records = ReadPackageIndexRecords(root, package);
    bool found = false;
    std::string artifact_path;
    for (nlohmann::json &record : records) {
      if (record.value("version", "") == version) {
        found = true;
        record["yanked"] = yanked;
        record["yanked_reason"] = yanked ? request.body.value("reason", std::string()) : "";
        if (yanked) {
          record["yanked_at"] = UtcTimestampNow();
        }
        else {
          record.erase("yanked_at");
        }
        artifact_path = record.at("source_artifact").at("path").get<std::string>();
      }
    }
    if (!found) {
      RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "not_found");
      return FailureResponse(404, "package release mutation failed", "package release is not found", "NotFound", operation);
    }
    WritePackageIndexRecords(root, package, records);
    const std::map<std::string, RegistryRoleKey> role_keys = LoadOrCreateRegistryRoleKeys(fs::path(config_.registry.key_dir));
    const std::string changed_at = UtcTimestampNow();
    const MetadataVersions metadata_versions = RefreshSignedRegistryMetadata(config_, role_keys, changed_at);
    const nlohmann::json publication =
      CreateRegistryPublication(yanked ? "yank" : "unyank", RegistryReleaseKey(package, version), changed_at);
    const std::string yank_reason = yanked ? request.body.value("reason", std::string()) : std::string();
    if (postgres_ != nullptr) {
      postgres_->SetRegistryPackageReleaseYanked(RegistryPackageId(package), version, yanked, yank_reason);
    }
    memory_.SetRegistryPackageReleaseYanked(RegistryPackageId(package), version, yanked, yank_reason);
    RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "success");
    if (UsesS3ObjectStore(config_)) {
      UploadRegistryTreeToS3(config_);
    }
    return JsonResponse(
      200,
      SuccessEnvelope(
        yanked ? "yanked package release" : "unyanked package release",
        {
          {"package_id", package},
          {"version", version},
          {"yanked", yanked},
          {"artifact_path", artifact_path},
          {"artifact_preserved", fs::exists(root / artifact_path)},
          {"checkpoint_version", metadata_versions.checkpoint_version},
          {"snapshot_version", metadata_versions.snapshot_version},
          {"timestamp_version", metadata_versions.timestamp_version},
          {"repository_version_id", publication.at("repository_version_id")},
          {"publication_id", publication.at("publication_id")},
          {"distribution_id", "default"},
        }
      )
    );
  }
  catch (const std::exception &error) {
    RecordRegistryAudit(request, operation, {{"package_id", package}, {"version", version}}, "failed");
    return FailureResponse(422, "package release mutation failed", error.what(), "PublishError", operation);
  }
}

}  // namespace spio::platform
