#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryControlPlaneSupport.hpp"

namespace spio::platform
{

HttpResponse
PlatformRouter::HandleMirrorStatus(const RouteMatch &match) const {
  const std::string mirror_id = match.parameters.at("mirror_id");
  if (postgres_ != nullptr) {
    try {
      const std::optional<MirrorCursorRecord> mirror = postgres_->GetMirrorState(mirror_id);
      if (!mirror.has_value()) {
        return FailureResponse(
          404,
          "mirror freshness unavailable",
          "mirror cursor not found",
          "MirrorError",
          "mirrorStatus"
        );
      }
      return JsonResponse(
        200,
        SuccessEnvelope("loaded mirror freshness", SerializeMirrorCursorRecord(*mirror))
      );
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "mirror freshness unavailable", error.what(), "PostgresError", "mirrorStatus");
    }
  }
  const std::optional<MirrorCursorRecord> mirror = memory_.GetMirrorState(mirror_id);
  if (!mirror.has_value()) {
    return FailureResponse(
      404,
      "mirror freshness unavailable",
      "mirror cursor not found",
      "MirrorError",
      "mirrorStatus"
    );
  }
  nlohmann::json payload = SerializeMirrorCursorRecord(*mirror);
  return JsonResponse(200, SuccessEnvelope("loaded mirror freshness", payload));
}

bool
PlatformRouter::RegistryWriteAuthorized(
  const HttpRequest &request,
  std::string_view scope,
  const std::string &package_id
) const {
  if (request.identity.has_value()) {
    const MtlsIdentity &identity = *request.identity;
    if (identity.role == "operator") {
      return true;
    }
    if (scope == "package:publish" && identity.role == "registry-writer") {
      return true;
    }
    if ((scope == "package:publish" || scope == "package:yank" || scope == "package:owner") && !package_id.empty()) {
      if (postgres_ != nullptr && postgres_->HasRegistryPackageOwner(package_id, identity.node_id)) {
        return true;
      }
      if (memory_.HasRegistryPackageOwner(package_id, identity.node_id)) {
        return true;
      }
    }
  }

  const std::optional<std::string> clear_token = RegistryTokenFromHeaders(request.headers);
  if (!clear_token.has_value()) {
    return false;
  }
  const std::optional<RegistryPublishTokenRecord> token = postgres_ != nullptr
                                                            ? postgres_->FindRegistryPublishTokenByHash(Sha256Bytes(*clear_token))
                                                            : memory_.FindRegistryPublishTokenByHash(Sha256Bytes(*clear_token));
  if (!token.has_value()) {
    return false;
  }
  const std::string now = UtcTimestampNow();
  if (!token->revoked_at.empty() || (!token->expires_at.empty() && token->expires_at < now)) {
    return false;
  }
  return TokenHasScope(*token, scope) && TokenMatchesPackage(*token, package_id);
}

void
PlatformRouter::RecordRegistryAudit(
  const HttpRequest &request,
  const std::string &operation,
  const nlohmann::json &target,
  const std::string &result
) {
  RegistryAuditEventRecord record{
    .event_id = postgres_ != nullptr ? postgres_->NextRegistryAuditEventId() : memory_.NextRegistryAuditEventId(),
    .actor_id = RegistryActorId(request),
    .operation = operation,
    .target = target,
    .request_id = request.headers.contains("x-request-id") ? request.headers.at("x-request-id") : "",
    .result = result,
    .created_at = UtcTimestampNow(),
  };
  if (postgres_ != nullptr) {
    postgres_->RecordRegistryAuditEvent(record);
  }
  memory_.RecordRegistryAuditEvent(record);
}

nlohmann::json
PlatformRouter::CreateRegistryPublication(
  const std::string &change_kind,
  const std::string &change_ref,
  const std::string &generated_at
) {
  const fs::path registry_root(config_.registry.root);
  fs::create_directories(registry_root / "_publications");
  fs::create_directories(registry_root / "_distributions" / "default");

  const size_t disk_sequence = NextPublicationSequenceOnDisk(registry_root);
  const int memory_sequence = memory_.NextRepositoryVersionSequence("default");
  const int persistent_sequence = postgres_ != nullptr ? postgres_->NextRepositoryVersionSequence("default") : memory_sequence;
  const int sequence = static_cast<int>(std::max(disk_sequence, static_cast<size_t>(memory_sequence)));
  const int publication_sequence = std::max(sequence, persistent_sequence);
  const std::string repository_version_id = "rv-" + PaddedNumber(static_cast<size_t>(publication_sequence), 6);
  const std::string publication_id = "pub-" + PaddedNumber(static_cast<size_t>(publication_sequence), 6);

  const fs::path temp_root = registry_root / "_tmp" / "publications" / publication_id;
  const fs::path final_root = registry_root / "_publications" / publication_id;
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(temp_root.parent_path());
  CopyRegistryReadPlaneTo(registry_root, temp_root);
  WriteTextFile(
    temp_root / "config.json",
    JsonText(RegistryConfigPayload(config_, generated_at, publication_id, repository_version_id))
  );

  const size_t tree_size = LeafSequencePaths(temp_root).size();
  const size_t artifact_count = CountRegularFiles(temp_root / "artifacts");
  nlohmann::json publication = {
    {"publication_id", publication_id},
    {"repository_id", "default"},
    {"repository_version_id", repository_version_id},
    {"layout_version", 2},
    {"generated_at", generated_at},
    {"tree_size", static_cast<int64_t>(tree_size)},
    {"index_digest", DirectoryDigest(temp_root / "index")},
    {"trust_digest", DirectoryDigest(temp_root / "trust")},
    {"artifact_count", static_cast<int64_t>(artifact_count)},
    {"verified", true},
  };
  WriteTextFile(temp_root / "publication.json", JsonText(publication));
  if (!fs::exists(temp_root / "config.json") || !fs::exists(temp_root / "trust" / "root.json") || !fs::exists(temp_root / "publication.json")) {
    throw std::runtime_error("publication verification failed before distribution update");
  }

  if (fs::exists(final_root, ec)) {
    throw std::runtime_error("publication already exists: " + publication_id);
  }
  fs::rename(temp_root, final_root, ec);
  if (ec) {
    throw std::runtime_error("failed to publish immutable publication: " + ec.message());
  }
  fs::copy_file(final_root / "config.json", registry_root / "config.json", fs::copy_options::overwrite_existing, ec);
  if (ec) {
    throw std::runtime_error("failed to activate publication config: " + ec.message());
  }

  const std::optional<nlohmann::json> previous = CurrentDistributionPointer(registry_root, "default");
  const std::string previous_publication_id =
    previous.has_value() ? previous->value("publication_id", std::string()) : std::string();
  nlohmann::json current = {
    {"distribution_id", "default"},
    {"publication_id", publication_id},
    {"repository_version_id", repository_version_id},
    {"updated_at", generated_at},
  };
  if (!previous_publication_id.empty()) {
    current["previous_publication_id"] = previous_publication_id;
  }
  WriteTextFile(registry_root / "_distributions" / "default" / "current.json", JsonText(current));

  memory_.UpsertRegistryRepository({
    .repository_id = "default",
    .name = "default",
    .tenant_id = "platform",
    .policy = {{"immutable_artifacts", true}, {"append_only_versions", true}},
    .created_at = generated_at,
  });
  memory_.RecordRegistryRepositoryVersion({
    .repository_version_id = repository_version_id,
    .repository_id = "default",
    .sequence = publication_sequence,
    .change_kind = change_kind,
    .change_ref = change_ref,
    .created_at = generated_at,
  });
  memory_.RecordRegistryPublication({
    .publication_id = publication_id,
    .repository_version_id = repository_version_id,
    .layout_version = 2,
    .root_path = (registry_root / "_publications" / publication_id).string(),
    .manifest_sha256 = spio::Sha256File(final_root / "publication.json"),
    .tree_size = static_cast<int>(tree_size),
    .created_at = generated_at,
    .verified = true,
  });
  memory_.UpsertRegistryDistribution({
    .distribution_id = "default",
    .repository_id = "default",
    .name = "default",
    .base_url = RegistryReadRootUrl(config_),
    .current_publication_id = publication_id,
    .previous_publication_id = previous_publication_id,
    .updated_at = generated_at,
  });
  if (postgres_ != nullptr) {
    postgres_->UpsertRegistryRepository({
      .repository_id = "default",
      .name = "default",
      .tenant_id = "platform",
      .policy = {{"immutable_artifacts", true}, {"append_only_versions", true}},
      .created_at = generated_at,
    });
    postgres_->RecordRegistryRepositoryVersion({
      .repository_version_id = repository_version_id,
      .repository_id = "default",
      .sequence = publication_sequence,
      .change_kind = change_kind,
      .change_ref = change_ref,
      .created_at = generated_at,
    });
    postgres_->RecordRegistryPublication({
      .publication_id = publication_id,
      .repository_version_id = repository_version_id,
      .layout_version = 2,
      .root_path = (registry_root / "_publications" / publication_id).string(),
      .manifest_sha256 = spio::Sha256File(final_root / "publication.json"),
      .tree_size = static_cast<int>(tree_size),
      .created_at = generated_at,
      .verified = true,
    });
    postgres_->UpsertRegistryDistribution({
      .distribution_id = "default",
      .repository_id = "default",
      .name = "default",
      .base_url = RegistryReadRootUrl(config_),
      .current_publication_id = publication_id,
      .previous_publication_id = previous_publication_id,
      .updated_at = generated_at,
    });
  }
  RecordMirrorState(
    "fresh",
    "checkpoint-" + PaddedNumber(tree_size, 4),
    publication_id,
    repository_version_id,
    generated_at,
    static_cast<int>(tree_size)
  );

  publication["manifest_sha256"] = spio::Sha256File(final_root / "publication.json");
  publication["root_path"] = (registry_root / "_publications" / publication_id).generic_string();
  publication["distribution"] = current;
  return publication;
}

HttpResponse
PlatformRouter::HandleRegistryStatus() const {
  if (UsesS3ObjectStore(config_)) {
    try {
      const bool config_present = ObjectExists(config_.object_store, "config.json");
      const bool root_metadata_present = ObjectExists(config_.object_store, "trust/root.json");
      nlohmann::json payload = {
        {"registry_root", "<redacted>"},
        {"key_dir", "<redacted>"},
        {"registry_name", config_.registry.registry_name},
        {"object_store_provider", config_.object_store.provider},
        {"root_initialized", config_present && root_metadata_present},
        {"config_present", config_present},
        {"root_metadata_present", root_metadata_present},
        {"publish_endpoint", "/api/spio-registry-control/v1/publish"},
        {"verify_endpoint", "/api/spio-registry-control/v1/verify"},
        {"descriptor_endpoint", "/api/spio-registry-control/v1/descriptor"},
      };
      return JsonResponse(200, SuccessEnvelope("registry control plane is ready", payload));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "registry status failed", error.what(), "RegistryStatusError", "registryStatus");
    }
  }

  const fs::path registry_root(config_.registry.root);
  const fs::path key_dir(config_.registry.key_dir);
  std::error_code ec;
  if (fs::exists(registry_root, ec) && !fs::is_directory(registry_root, ec)) {
    return FailureResponse(
      503,
      "registry status failed",
      "registry root is not a directory",
      "RegistryStatusError",
      "registryStatus"
    );
  }
  if (fs::exists(key_dir, ec) && !fs::is_directory(key_dir, ec)) {
    return FailureResponse(
      503,
      "registry status failed",
      "registry key directory is not a directory",
      "RegistryStatusError",
      "registryStatus"
    );
  }

  const bool config_present = fs::exists(registry_root / "config.json", ec);
  const bool root_metadata_present = fs::exists(registry_root / "trust" / "root.json", ec);
  nlohmann::json payload = {
    {"registry_root", "<redacted>"},
    {"key_dir", "<redacted>"},
    {"registry_name", config_.registry.registry_name},
    {"root_initialized", config_present && root_metadata_present},
    {"config_present", config_present},
    {"root_metadata_present", root_metadata_present},
    {"publish_endpoint", "/api/spio-registry-control/v1/publish"},
    {"verify_endpoint", "/api/spio-registry-control/v1/verify"},
    {"descriptor_endpoint", "/api/spio-registry-control/v1/descriptor"},
  };
  return JsonResponse(200, SuccessEnvelope("registry control plane is ready", payload));
}

HttpResponse
PlatformRouter::HandleRegistryDescriptor() const {
  if (UsesS3ObjectStore(config_)) {
    try {
      const std::optional<std::string> root_metadata = GetObjectText(config_.object_store, "trust/root.json");
      if (!root_metadata.has_value()) {
        return FailureResponse(
          422,
          "registry descriptor failed",
          "registry root metadata is not initialized",
          "RegistryDescriptorError",
          "registryDescriptor"
        );
      }
      nlohmann::json payload = {
        {"schema_version", 1},
        {"registry_name", config_.registry.registry_name},
        {"registry_root", RegistryReadRootUrl(config_)},
        {"control_plane_base_url", RegistryControlPlaneBaseUrl(config_)},
        {"root_sha256", Sha256Bytes(*root_metadata)},
        {"issued_at", UtcTimestampNow()},
        {"expires", UtcTimestampPlusDays(31)},
        {"descriptor_signature", "platform-control-plane-mtls"},
      };
      return JsonResponse(200, SuccessEnvelope("published registry trust descriptor", payload));
    }
    catch (const std::exception &error) {
      return FailureResponse(503, "registry descriptor failed", error.what(), "RegistryDescriptorError", "registryDescriptor");
    }
  }

  const fs::path registry_root(config_.registry.root);
  const fs::path root_metadata = registry_root / "trust" / "root.json";
  std::error_code ec;
  if (!fs::exists(root_metadata, ec)) {
    return FailureResponse(
      422,
      "registry descriptor failed",
      "registry root metadata is not initialized",
      "RegistryDescriptorError",
      "registryDescriptor"
    );
  }
  nlohmann::json payload = {
    {"schema_version", 1},
    {"registry_name", config_.registry.registry_name},
    {"registry_root", RegistryReadRootUrl(config_)},
    {"control_plane_base_url", RegistryControlPlaneBaseUrl(config_)},
    {"root_sha256", spio::Sha256File(root_metadata)},
    {"issued_at", "2026-05-02T00:00:00Z"},
    {"expires", "2026-06-02T00:00:00Z"},
    {"descriptor_signature", "platform-control-plane-mtls"},
  };
  return JsonResponse(200, SuccessEnvelope("published registry trust descriptor", payload));
}

void
PlatformRouter::RecordMirrorState(
  std::string freshness,
  std::string replay_cursor,
  std::string publication_id,
  std::string repository_version_id,
  std::string synced_at,
  const int tree_size
) {
  if (postgres_ != nullptr) {
    postgres_->RecordMirrorState(
      config_.registry.mirror_id,
      config_.region,
      config_.registry.mirror_origin,
      freshness,
      replay_cursor,
      publication_id,
      repository_version_id,
      synced_at,
      tree_size
    );
  }
  memory_.RecordMirrorState(
    config_.registry.mirror_id,
    config_.region,
    config_.registry.mirror_origin,
    freshness,
    replay_cursor,
    publication_id,
    repository_version_id,
    synced_at,
    tree_size
  );
}

}  // namespace spio::platform
