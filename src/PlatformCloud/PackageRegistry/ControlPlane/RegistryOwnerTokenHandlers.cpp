#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryControlPlaneSupport.hpp"

namespace spio::platform
{

HttpResponse
PlatformRouter::HandleListPackageOwners(const RouteMatch &match) const {
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
    return FailureResponse(400, "package owner lookup rejected", *error, "ValidationError", "listPackageOwners", 2);
  }
  if (ReadPackageIndexRecords(fs::path(config_.registry.root), package).empty()) {
    return FailureResponse(404, "package owner lookup failed", "package is not found", "NotFound", "listPackageOwners");
  }
  nlohmann::json owners = postgres_ != nullptr
                            ? postgres_->ListRegistryPackageOwners(RegistryPackageId(package))
                            : memory_.ListRegistryPackageOwners(RegistryPackageId(package));
  if (owners.empty()) {
    const std::vector<nlohmann::json> releases = ReadPackageIndexRecords(fs::path(config_.registry.root), package);
    if (!releases.empty()) {
      owners.push_back({
        {"package_id", package},
        {"owner_id", releases.front().value("publisher_id", "")},
        {"owner_kind", "user"},
        {"role", "owner"},
        {"added_by", "registry-index"},
        {"added_at", releases.front().value("published_at", "")},
      });
    }
  }
  return JsonResponse(200, SuccessEnvelope("loaded package owners", {{"package_id", package}, {"owners", owners}}));
}

HttpResponse
PlatformRouter::HandleAddPackageOwner(const RouteMatch &match, const HttpRequest &request) {
  const std::string package = RegistryPackageFromRoute(match);
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
    return FailureResponse(400, "package owner mutation rejected", *error, "ValidationError", "addPackageOwner", 2);
  }
  if (!request.body.is_object() || !HasNonEmptyString(request.body, "owner_id")) {
    return FailureResponse(400, "package owner mutation rejected", "owner_id is required", "ValidationError", "addPackageOwner", 2);
  }
  if (!RegistryWriteAuthorized(request, "package:owner", RegistryPackageId(package))) {
    RecordRegistryAudit(request, "addPackageOwner", {{"package_id", package}, {"owner_id", request.body.value("owner_id", "")}}, "denied");
    return FailureResponse(403, "package owner mutation denied", "token or identity lacks package:owner", "AuthError", "addPackageOwner", 2);
  }
  if (ReadPackageIndexRecords(fs::path(config_.registry.root), package).empty()) {
    RecordRegistryAudit(request, "addPackageOwner", {{"package_id", package}, {"owner_id", request.body.value("owner_id", "")}}, "not_found");
    return FailureResponse(404, "package owner mutation failed", "package is not found", "NotFound", "addPackageOwner");
  }
  RegistryPackageOwnerRecord owner{
    .package_id = package,
    .owner_id = request.body.at("owner_id").get<std::string>(),
    .owner_kind = request.body.value("owner_kind", "user"),
    .role = request.body.value("role", "owner"),
    .added_by = RegistryActorId(request),
    .added_at = UtcTimestampNow(),
  };
  if (postgres_ != nullptr) {
    postgres_->AddRegistryPackageOwner(owner);
  }
  memory_.AddRegistryPackageOwner(owner);
  RecordRegistryAudit(request, "addPackageOwner", {{"package_id", package}, {"owner_id", owner.owner_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("added package owner", SerializeRegistryPackageOwnerRecord(owner)));
}

HttpResponse
PlatformRouter::HandleRemovePackageOwner(const RouteMatch &match, const HttpRequest &request) {
  const std::string package = RegistryPackageFromRoute(match);
  const std::string owner_id = match.parameters.at("owner_id");
  if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
    return FailureResponse(400, "package owner mutation rejected", *error, "ValidationError", "removePackageOwner", 2);
  }
  if (!RegistryWriteAuthorized(request, "package:owner", RegistryPackageId(package))) {
    RecordRegistryAudit(request, "removePackageOwner", {{"package_id", package}, {"owner_id", owner_id}}, "denied");
    return FailureResponse(403, "package owner mutation denied", "token or identity lacks package:owner", "AuthError", "removePackageOwner", 2);
  }
  const bool removed = postgres_ != nullptr
                         ? postgres_->RemoveRegistryPackageOwner(RegistryPackageId(package), owner_id)
                         : memory_.RemoveRegistryPackageOwner(RegistryPackageId(package), owner_id);
  if (removed) {
    memory_.RemoveRegistryPackageOwner(RegistryPackageId(package), owner_id);
  }
  if (!removed) {
    RecordRegistryAudit(request, "removePackageOwner", {{"package_id", package}, {"owner_id", owner_id}}, "not_found");
    return FailureResponse(404, "package owner mutation failed", "package owner is not found", "NotFound", "removePackageOwner");
  }
  RecordRegistryAudit(request, "removePackageOwner", {{"package_id", package}, {"owner_id", owner_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("removed package owner", {{"package_id", package}, {"owner_id", owner_id}}));
}

HttpResponse
PlatformRouter::HandleCreatePublishToken(const HttpRequest &request) {
  if (!request.body.is_object()) {
    return FailureResponse(400, "publish token creation rejected", "request body must be an object", "ValidationError", "createPublishToken", 2);
  }
  if (const std::optional<std::string> error = ValidateStringArray(request.body, "scopes"); error.has_value()) {
    return FailureResponse(400, "publish token creation rejected", *error, "ValidationError", "createPublishToken", 2);
  }
  if (const std::optional<std::string> error = ValidateStringArray(request.body, "package_patterns"); error.has_value()) {
    return FailureResponse(400, "publish token creation rejected", *error, "ValidationError", "createPublishToken", 2);
  }
  const std::string actor = RegistryActorId(request);
  const std::string owner_id = request.body.value("owner_id", actor);
  if (!request.identity.has_value() || (request.identity->role != "operator" && owner_id != actor)) {
    RecordRegistryAudit(request, "createPublishToken", {{"owner_id", owner_id}}, "denied");
    return FailureResponse(403, "publish token creation denied", "token owner must match caller", "AuthError", "createPublishToken", 2);
  }
  const std::string token_id = postgres_ != nullptr ? postgres_->NextRegistryTokenId() : memory_.NextRegistryTokenId();
  const std::string clear_token = "styio_pat_" + token_id + "_" + SecureRandomHex(24);
  RegistryPublishTokenRecord token{
    .token_id = token_id,
    .token_hash = Sha256Bytes(clear_token),
    .owner_id = owner_id,
    .scopes = JsonStringArray(request.body, "scopes", {"package:publish"}),
    .package_patterns = JsonStringArray(request.body, "package_patterns", {"*"}),
    .expires_at = request.body.value("expires_at", ""),
    .revoked_at = "",
    .created_at = UtcTimestampNow(),
  };
  if (postgres_ != nullptr) {
    postgres_->UpsertRegistryPublishToken(token);
  }
  memory_.UpsertRegistryPublishToken(token);
  nlohmann::json payload = SerializeRegistryPublishTokenRecord(token);
  payload["token"] = clear_token;
  RecordRegistryAudit(request, "createPublishToken", {{"token_id", token_id}, {"owner_id", owner_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("created publish token", payload));
}

HttpResponse
PlatformRouter::HandleListPublishTokens(const HttpRequest &request) const {
  const std::string owner_filter =
    request.identity.has_value() && request.identity->role == "operator" ? std::string() : RegistryActorId(request);
  return JsonResponse(
    200,
    SuccessEnvelope(
      "loaded publish tokens",
      {{"tokens", postgres_ != nullptr ? postgres_->ListRegistryPublishTokens(owner_filter) : memory_.ListRegistryPublishTokens(owner_filter)}}
    )
  );
}

HttpResponse
PlatformRouter::HandleRevokePublishToken(const RouteMatch &match, const HttpRequest &request) {
  const std::string token_id = match.parameters.at("token_id");
  const std::optional<RegistryPublishTokenRecord> token = postgres_ != nullptr
                                                            ? postgres_->GetRegistryPublishToken(token_id)
                                                            : memory_.GetRegistryPublishToken(token_id);
  if (!token.has_value()) {
    RecordRegistryAudit(request, "revokePublishToken", {{"token_id", token_id}}, "not_found");
    return FailureResponse(404, "publish token revocation failed", "token is not found", "NotFound", "revokePublishToken");
  }
  const std::string actor = RegistryActorId(request);
  if (!request.identity.has_value() || (request.identity->role != "operator" && token->owner_id != actor)) {
    RecordRegistryAudit(request, "revokePublishToken", {{"token_id", token_id}}, "denied");
    return FailureResponse(403, "publish token revocation denied", "token owner must match caller", "AuthError", "revokePublishToken", 2);
  }
  const std::string revoked_at = UtcTimestampNow();
  if (postgres_ != nullptr) {
    postgres_->RevokeRegistryPublishToken(token_id, revoked_at);
  }
  memory_.RevokeRegistryPublishToken(token_id, revoked_at);
  RecordRegistryAudit(request, "revokePublishToken", {{"token_id", token_id}}, "success");
  return JsonResponse(200, SuccessEnvelope("revoked publish token", {{"token_id", token_id}, {"revoked", true}}));
}

}  // namespace spio::platform
