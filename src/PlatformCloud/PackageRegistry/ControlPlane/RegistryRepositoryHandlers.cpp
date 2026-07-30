#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryControlPlaneSupport.hpp"
#include "PlatformCloud/PackageRegistry/ReleaseManagement/ReleaseManager.hpp"

namespace pafio::platform
{

HttpResponse
PlatformRouter::HandleListRepositories() const {
  nlohmann::json repositories =
    postgres_ != nullptr ? postgres_->ListRegistryRepositories() : memory_.ListRegistryRepositories();
  if (repositories.empty()) {
    repositories.push_back({
      {"repository_id", "default"},
      {"name", "default"},
      {"tenant_id", "platform"},
      {"policy", {{"immutable_artifacts", true}, {"append_only_versions", true}}},
      {"created_at", ""},
    });
  }
  return JsonResponse(200, SuccessEnvelope("loaded repositories", {{"repositories", repositories}}));
}

HttpResponse
PlatformRouter::HandleListRepositoryVersions(const RouteMatch &match) const {
  const std::string repository_id = match.parameters.at("repository_id");
  return JsonResponse(
    200,
    SuccessEnvelope(
      "loaded repository versions",
      {{"repository_id", repository_id},
       {"versions", postgres_ != nullptr ? postgres_->ListRegistryRepositoryVersions(repository_id) : memory_.ListRegistryRepositoryVersions(repository_id)}}
    )
  );
}

HttpResponse
PlatformRouter::HandleGetPublication(const RouteMatch &match) const {
  const std::string publication_id = match.parameters.at("publication_id");
  const fs::path publication_path = fs::path(config_.registry.root) / "_publications" / publication_id / "publication.json";
  if (!fs::exists(publication_path)) {
    return FailureResponse(404, "publication lookup failed", "publication is not found", "NotFound", "getPublication");
  }
  nlohmann::json payload = nlohmann::json::parse(ReadFileBytes(publication_path));
  const std::optional<RegistryPublicationRecord> record =
    postgres_ != nullptr ? postgres_->GetRegistryPublication(publication_id) : memory_.GetRegistryPublication(publication_id);
  if (record.has_value()) {
    payload["control_plane_record"] = SerializeRegistryPublicationRecord(*record);
  }
  return JsonResponse(200, SuccessEnvelope("loaded publication", payload));
}

HttpResponse
PlatformRouter::HandleVerifyPublication(const RouteMatch &match, const HttpRequest &request) {
  if (!request.body.is_object() || !request.body.empty()) {
    return FailureResponse(400, "publication verification rejected", "verify request must be an empty JSON object", "VerifyError", "verifyPublication", 2);
  }
  const std::string publication_id = match.parameters.at("publication_id");
  const fs::path publication_root = fs::path(config_.registry.root) / "_publications" / publication_id;
  const fs::path publication_path = publication_root / "publication.json";
  if (!fs::exists(publication_path)) {
    return FailureResponse(404, "publication verification failed", "publication is not found", "NotFound", "verifyPublication");
  }
  try {
    nlohmann::json publication = nlohmann::json::parse(ReadFileBytes(publication_path));
    const bool index_ok = publication.value("index_digest", "") == DirectoryDigest(publication_root / "index");
    const bool trust_ok = publication.value("trust_digest", "") == DirectoryDigest(publication_root / "trust");
    if (!index_ok || !trust_ok) {
      return FailureResponse(422, "publication verification failed", "publication digest mismatch", "VerifyError", "verifyPublication");
    }
    publication["verified"] = true;
    return JsonResponse(200, SuccessEnvelope("verified publication", publication));
  }
  catch (const std::exception &error) {
    return FailureResponse(422, "publication verification failed", error.what(), "VerifyError", "verifyPublication");
  }
}

HttpResponse
PlatformRouter::HandleListDistributions() const {
  nlohmann::json distributions =
    postgres_ != nullptr ? postgres_->ListRegistryDistributions() : memory_.ListRegistryDistributions();
  if (distributions.empty()) {
    const std::optional<nlohmann::json> current = CurrentDistributionPointer(fs::path(config_.registry.root), "default");
    distributions.push_back({
      {"distribution_id", "default"},
      {"repository_id", "default"},
      {"name", "default"},
      {"base_url", RegistryReadRootUrl(config_)},
      {"current_publication_id", current.has_value() ? current->value("publication_id", "") : ""},
      {"previous_publication_id", current.has_value() ? current->value("previous_publication_id", "") : ""},
      {"updated_at", current.has_value() ? current->value("updated_at", "") : ""},
    });
  }
  return JsonResponse(200, SuccessEnvelope("loaded distributions", {{"distributions", distributions}}));
}

HttpResponse
PlatformRouter::HandleListReleaseChannels() const {
  return JsonResponse(
    200,
    SuccessEnvelope("loaded release channels", {{"channels", ListReleaseChannels(fs::path(config_.registry.root))}})
  );
}

HttpResponse
PlatformRouter::HandleRolloutReleaseChannel(const RouteMatch &match, const HttpRequest &request) {
  if (!request.body.is_object()) {
    return FailureResponse(400, "release rollout rejected", "request body must be an object", "ValidationError", "rolloutReleaseChannel", 2);
  }
  const std::string channel = match.parameters.at("channel");
  const std::string publication_id = request.body.value("publication_id", "");
  const fs::path publication_path = fs::path(config_.registry.root) / "_publications" / publication_id / "publication.json";
  if (publication_id.empty() || !fs::exists(publication_path)) {
    return FailureResponse(404, "release rollout failed", "publication is not found", "NotFound", "rolloutReleaseChannel");
  }
  try {
    const nlohmann::json plan = BuildReleaseRolloutPlan(
      channel,
      publication_id,
      request.body.value("percentage", 0),
      request.body.value("ring", "default"),
      UtcTimestampNow()
    );
    WriteReleaseChannel(fs::path(config_.registry.root), plan);
    RecordRegistryAudit(request, "rolloutReleaseChannel", {{"channel", channel}, {"publication_id", publication_id}}, "success");
    return JsonResponse(200, SuccessEnvelope("updated release channel rollout", plan));
  }
  catch (const std::exception &error) {
    RecordRegistryAudit(request, "rolloutReleaseChannel", {{"channel", channel}, {"publication_id", publication_id}}, "failed");
    return FailureResponse(422, "release rollout failed", error.what(), "ReleaseManagementError", "rolloutReleaseChannel");
  }
}

HttpResponse
PlatformRouter::HandlePromoteDistribution(const RouteMatch &match, const HttpRequest &request) {
  const std::string distribution_id = match.parameters.at("distribution_id");
  if (!request.body.is_object() || !HasNonEmptyString(request.body, "publication_id")) {
    return FailureResponse(400, "distribution promotion rejected", "publication_id is required", "ValidationError", "promoteDistribution", 2);
  }
  if (!RegistryWriteAuthorized(request, "repository:promote", "")) {
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}}, "denied");
    return FailureResponse(403, "distribution promotion denied", "token or identity lacks repository:promote", "AuthError", "promoteDistribution", 2);
  }
  const std::string publication_id = request.body.at("publication_id").get<std::string>();
  const fs::path registry_root(config_.registry.root);
  const fs::path publication_root = registry_root / "_publications" / publication_id;
  const fs::path publication_path = publication_root / "publication.json";
  if (!fs::exists(publication_path)) {
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}, {"publication_id", publication_id}}, "not_found");
    return FailureResponse(404, "distribution promotion failed", "publication is not found", "NotFound", "promoteDistribution");
  }
  try {
    const nlohmann::json publication = nlohmann::json::parse(ReadFileBytes(publication_path));
    const std::optional<nlohmann::json> previous = CurrentDistributionPointer(registry_root, distribution_id);
    if (distribution_id == "default") {
      MaterializePublicationToRoot(registry_root, publication_root);
    }
    const std::string updated_at = UtcTimestampNow();
    const std::string previous_publication_id =
      previous.has_value() ? previous->value("publication_id", std::string()) : std::string();
    nlohmann::json current = {
      {"distribution_id", distribution_id},
      {"publication_id", publication_id},
      {"repository_version_id", publication.at("repository_version_id").get<std::string>()},
      {"updated_at", updated_at},
    };
    if (!previous_publication_id.empty()) {
      current["previous_publication_id"] = previous_publication_id;
    }
    WriteTextFile(registry_root / "_distributions" / distribution_id / "current.json", JsonText(current));
    RegistryDistributionRecord distribution_record{
      .distribution_id = distribution_id,
      .repository_id = publication.value("repository_id", "default"),
      .name = distribution_id,
      .base_url = RegistryReadRootUrl(config_),
      .current_publication_id = publication_id,
      .previous_publication_id = previous_publication_id,
      .updated_at = updated_at,
    };
    if (postgres_ != nullptr) {
      postgres_->UpsertRegistryDistribution(distribution_record);
    }
    memory_.UpsertRegistryDistribution(distribution_record);
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}, {"publication_id", publication_id}}, "success");
    return JsonResponse(200, SuccessEnvelope("promoted distribution", current));
  }
  catch (const std::exception &error) {
    RecordRegistryAudit(request, "promoteDistribution", {{"distribution_id", distribution_id}, {"publication_id", publication_id}}, "failed");
    return FailureResponse(422, "distribution promotion failed", error.what(), "PublishError", "promoteDistribution");
  }
}

HttpResponse
PlatformRouter::HandleRollbackDistribution(const RouteMatch &match, const HttpRequest &request) {
  const std::string distribution_id = match.parameters.at("distribution_id");
  if (!RegistryWriteAuthorized(request, "repository:promote", "")) {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}}, "denied");
    return FailureResponse(403, "distribution rollback denied", "token or identity lacks repository:promote", "AuthError", "rollbackDistribution", 2);
  }
  const fs::path registry_root(config_.registry.root);
  const std::optional<nlohmann::json> current_pointer = CurrentDistributionPointer(registry_root, distribution_id);
  if (!current_pointer.has_value() || current_pointer->value("previous_publication_id", std::string()).empty()) {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}}, "no_previous_publication");
    return FailureResponse(409, "distribution rollback failed", "previous publication is not available", "StateError", "rollbackDistribution");
  }
  const std::string rollback_publication_id = current_pointer->at("previous_publication_id").get<std::string>();
  const fs::path publication_root = registry_root / "_publications" / rollback_publication_id;
  const fs::path publication_path = publication_root / "publication.json";
  if (!fs::exists(publication_path)) {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}, {"publication_id", rollback_publication_id}}, "not_found");
    return FailureResponse(404, "distribution rollback failed", "previous publication is not found", "NotFound", "rollbackDistribution");
  }
  try {
    const nlohmann::json publication = nlohmann::json::parse(ReadFileBytes(publication_path));
    if (distribution_id == "default") {
      MaterializePublicationToRoot(registry_root, publication_root);
    }
    const std::string updated_at = UtcTimestampNow();
    nlohmann::json next_pointer = {
      {"distribution_id", distribution_id},
      {"publication_id", rollback_publication_id},
      {"repository_version_id", publication.at("repository_version_id").get<std::string>()},
      {"previous_publication_id", current_pointer->value("publication_id", "")},
      {"updated_at", updated_at},
    };
    WriteTextFile(registry_root / "_distributions" / distribution_id / "current.json", JsonText(next_pointer));
    RegistryDistributionRecord distribution_record{
      .distribution_id = distribution_id,
      .repository_id = publication.value("repository_id", "default"),
      .name = distribution_id,
      .base_url = RegistryReadRootUrl(config_),
      .current_publication_id = rollback_publication_id,
      .previous_publication_id = current_pointer->value("publication_id", ""),
      .updated_at = updated_at,
    };
    if (postgres_ != nullptr) {
      postgres_->UpsertRegistryDistribution(distribution_record);
    }
    memory_.UpsertRegistryDistribution(distribution_record);
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}, {"publication_id", rollback_publication_id}}, "success");
    return JsonResponse(200, SuccessEnvelope("rolled back distribution", next_pointer));
  }
  catch (const std::exception &error) {
    RecordRegistryAudit(request, "rollbackDistribution", {{"distribution_id", distribution_id}}, "failed");
    return FailureResponse(422, "distribution rollback failed", error.what(), "PublishError", "rollbackDistribution");
  }
}

}  // namespace pafio::platform
