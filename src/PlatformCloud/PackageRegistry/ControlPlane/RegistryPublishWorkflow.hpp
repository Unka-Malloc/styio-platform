#pragma once

#include <openssl/rand.h>

#include <toml++/toml.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryIndexStore.hpp"
#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryTrustMetadata.hpp"
#include "PlatformCore/Core/Errors.hpp"
#include "PlatformCore/Core/Sha256.hpp"

namespace spio::platform
{

namespace
{

constexpr uint64_t kMaxPublishArchiveBytes = 64U * 1024U * 1024U;
constexpr size_t kMaxPublishArchiveBase64Bytes =
  4U * ((static_cast<size_t>(kMaxPublishArchiveBytes) + 2U) / 3U);
constexpr size_t kMaxPublishDependenciesPerTable = 256U;
constexpr size_t kMaxPublishPackageBytes = 255U;
constexpr size_t kMaxPublishVersionBytes = 64U;
constexpr size_t kMaxPublishArchiveNameBytes = 255U;
constexpr size_t kMaxPublishPublisherBytes = 255U;
constexpr size_t kMaxPublishAliasBytes = 128U;
constexpr size_t kMaxPublishRegistryBytes = 2048U;
constexpr size_t kUstarBlockBytes = 512U;
constexpr uint64_t kMaxPafioManifestBytes = 1024U * 1024U;

struct PublishDraft
{
  std::string package;
  std::string version;
  std::string publisher_id;
  std::string archive_name;
  std::string archive_sha256;
  uint64_t archive_size_bytes = 0;
  nlohmann::json dependencies = nlohmann::json::array();
  nlohmann::json dev_dependencies = nlohmann::json::array();
  fs::path staging_root;
  fs::path staging_directory;
  fs::path archive_path;
  std::string manifest_digest;
  bool remove_empty_registry_root_on_cleanup = false;
};

class PublishStagingCleanup
{
public:
  explicit PublishStagingCleanup(PublishDraft &draft)
    : draft_(draft)
  {
  }

  PublishStagingCleanup(const PublishStagingCleanup &) = delete;
  PublishStagingCleanup &operator=(const PublishStagingCleanup &) = delete;

  ~PublishStagingCleanup() {
    if (!draft_.staging_directory.empty()) {
      std::error_code error;
      fs::remove_all(draft_.staging_directory, error);
    }
    if (draft_.staging_root.empty()) {
      return;
    }
    std::error_code error;
    fs::remove(draft_.staging_root, error);
    fs::remove(draft_.staging_root.parent_path(), error);
    if (draft_.remove_empty_registry_root_on_cleanup) {
      fs::remove(draft_.staging_root.parent_path().parent_path(), error);
    }
  }

private:
  PublishDraft &draft_;
};

void
ValidatePublishString(
  const nlohmann::json &object,
  const std::string &field,
  const size_t max_bytes
) {
  if (!object.contains(field) || !object.at(field).is_string()) {
    throw spio::ValidationError(field + " must be a string");
  }
  const std::string &value = object.at(field).get_ref<const std::string &>();
  if (value.empty()) {
    throw spio::ValidationError(field + " must not be empty");
  }
  if (value.size() > max_bytes) {
    throw spio::ValidationError(field + " exceeds the " + std::to_string(max_bytes) + "-byte limit");
  }
}

void
ValidateExactPublishFields(
  const nlohmann::json &object,
  const std::set<std::string> &required,
  const std::string &context
) {
  if (!object.is_object()) {
    throw spio::ValidationError(context + " must be a JSON object");
  }
  for (const std::string &field : required) {
    if (!object.contains(field)) {
      throw spio::ValidationError(context + " is missing required field: " + field);
    }
  }
  for (auto field = object.begin(); field != object.end(); ++field) {
    if (!required.contains(field.key())) {
      throw spio::ValidationError(context + " contains unknown field: " + field.key());
    }
  }
}

bool
IsLowerHexDigest(std::string_view value) {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](const unsigned char ch)
                     {
                       return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                     });
}

bool
IsSafePublishArchiveName(std::string_view value) {
  constexpr std::string_view suffix = ".pafio.src.tar";
  if (value.empty() || value.front() == '.' || !value.ends_with(suffix)) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const unsigned char ch)
                     {
                       return (ch >= 'A' && ch <= 'Z') ||
                              (ch >= 'a' && ch <= 'z') ||
                              (ch >= '0' && ch <= '9') ||
                              ch == '-' || ch == '_' || ch == '.';
                     });
}

int
PublishBase64Value(const unsigned char ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<int>(ch - 'A');
  }
  if (ch >= 'a' && ch <= 'z') {
    return static_cast<int>(ch - 'a') + 26;
  }
  if (ch >= '0' && ch <= '9') {
    return static_cast<int>(ch - '0') + 52;
  }
  if (ch == '+') {
    return 62;
  }
  if (ch == '/') {
    return 63;
  }
  return -1;
}

uint64_t
ValidateCanonicalPublishBase64(std::string_view encoded) {
  if (encoded.empty()) {
    throw spio::ValidationError("archive_base64 must not be empty");
  }
  if (encoded.size() > kMaxPublishArchiveBase64Bytes || encoded.size() % 4U != 0U) {
    throw spio::ValidationError("archive_base64 is not canonical base64");
  }

  size_t padding = 0;
  if (encoded.back() == '=') {
    padding = 1;
    if (encoded[encoded.size() - 2U] == '=') {
      padding = 2;
    }
  }
  for (size_t index = 0; index < encoded.size(); ++index) {
    const bool padding_position = index >= encoded.size() - padding;
    if (encoded[index] == '=') {
      if (!padding_position) {
        throw spio::ValidationError("archive_base64 is not canonical base64");
      }
      continue;
    }
    if (padding_position || PublishBase64Value(static_cast<unsigned char>(encoded[index])) < 0) {
      throw spio::ValidationError("archive_base64 is not canonical base64");
    }
  }

  if (padding == 2U) {
    const int second = PublishBase64Value(static_cast<unsigned char>(encoded[encoded.size() - 3U]));
    if ((second & 0x0F) != 0) {
      throw spio::ValidationError("archive_base64 is not canonical base64");
    }
  }
  else if (padding == 1U) {
    const int third = PublishBase64Value(static_cast<unsigned char>(encoded[encoded.size() - 2U]));
    if ((third & 0x03) != 0) {
      throw spio::ValidationError("archive_base64 is not canonical base64");
    }
  }

  const uint64_t decoded_size =
    static_cast<uint64_t>(encoded.size() / 4U) * 3U - static_cast<uint64_t>(padding);
  if (decoded_size == 0U || decoded_size > kMaxPublishArchiveBytes) {
    throw spio::ValidationError("decoded archive must be nonempty and no larger than 67108864 bytes");
  }
  return decoded_size;
}

std::string
DecodeCanonicalPublishBase64(std::string_view encoded, const uint64_t decoded_size) {
  std::string decoded;
  decoded.reserve(static_cast<size_t>(decoded_size));
  for (size_t offset = 0; offset < encoded.size(); offset += 4U) {
    const int first = PublishBase64Value(static_cast<unsigned char>(encoded[offset]));
    const int second = PublishBase64Value(static_cast<unsigned char>(encoded[offset + 1U]));
    decoded.push_back(static_cast<char>((first << 2) | (second >> 4)));
    if (encoded[offset + 2U] != '=') {
      const int third = PublishBase64Value(static_cast<unsigned char>(encoded[offset + 2U]));
      decoded.push_back(static_cast<char>(((second & 0x0F) << 4) | (third >> 2)));
      if (encoded[offset + 3U] != '=') {
        const int fourth = PublishBase64Value(static_cast<unsigned char>(encoded[offset + 3U]));
        decoded.push_back(static_cast<char>(((third & 0x03) << 6) | fourth));
      }
    }
  }
  if (decoded.size() != decoded_size) {
    throw spio::ValidationError("archive_base64 decoded size does not match archive_size_bytes");
  }
  return decoded;
}

bool
UstarBytesAreZero(std::string_view bytes) {
  return std::all_of(
    bytes.begin(),
    bytes.end(),
    [](const unsigned char byte)
    {
      return byte == 0U;
    }
  );
}

uint64_t
ParseCanonicalUstarOctal(std::string_view field, const std::string &name) {
  if (field.size() < 2U || field.back() != '\0') {
    throw spio::ValidationError("source archive " + name + " must be canonical NUL-terminated octal");
  }
  uint64_t value = 0U;
  for (size_t index = 0; index + 1U < field.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(field[index]);
    if (byte < '0' || byte > '7') {
      throw spio::ValidationError("source archive " + name + " must be canonical NUL-terminated octal");
    }
    const uint64_t digit = static_cast<uint64_t>(byte - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 8U) {
      throw spio::ValidationError("source archive " + name + " exceeds the supported numeric range");
    }
    value = value * 8U + digit;
  }
  return value;
}

uint64_t
ParseCanonicalUstarChecksum(std::string_view field) {
  if (field.size() != 8U || field[6] != '\0' || field[7] != ' ') {
    throw spio::ValidationError("source archive checksum field is not canonical ustar octal");
  }
  return ParseCanonicalUstarOctal(field.substr(0U, 7U), "checksum");
}

std::string
ParseCanonicalUstarText(std::string_view field, const std::string &name) {
  const size_t terminator = field.find('\0');
  if (terminator == std::string_view::npos) {
    return std::string(field);
  }
  if (!UstarBytesAreZero(field.substr(terminator))) {
    throw spio::ValidationError("source archive " + name + " has nonzero bytes after its terminator");
  }
  return std::string(field.substr(0U, terminator));
}

std::vector<std::string_view>
ValidateCanonicalUstarPath(const std::string &path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos) {
    throw spio::ValidationError("source archive member path must be canonical POSIX-relative");
  }
  std::vector<std::string_view> parts;
  size_t offset = 0U;
  while (offset <= path.size()) {
    const size_t separator = path.find('/', offset);
    const size_t end = separator == std::string::npos ? path.size() : separator;
    const std::string_view part(path.data() + offset, end - offset);
    if (part.empty() || part == "." || part == "..") {
      throw spio::ValidationError("source archive member path must not contain empty, dot, or parent segments");
    }
    parts.push_back(part);
    if (separator == std::string::npos) {
      break;
    }
    offset = separator + 1U;
  }
  if (parts.size() < 2U) {
    throw spio::ValidationError("source archive members must belong to one top-level package prefix");
  }
  return parts;
}

// Pafio source archives are a single canonical ustar byte representation.
// Rejecting alternate tar encodings keeps signatures and content hashes
// reproducible and prevents path or type ambiguity before publication.
std::string
ValidateDeterministicPafioUstar(std::string_view archive) {
  if (archive.size() < 3U * kUstarBlockBytes || archive.size() % kUstarBlockBytes != 0U) {
    throw spio::ValidationError("source archive must be 512-byte aligned canonical ustar");
  }

  std::set<std::string> paths;
  std::optional<std::string> top_level_prefix;
  std::optional<std::string> previous_path;
  std::string manifest_bytes;
  size_t manifest_count = 0U;
  size_t offset = 0U;
  bool trailer_seen = false;
  while (offset < archive.size()) {
    const std::string_view header = archive.substr(offset, kUstarBlockBytes);
    if (UstarBytesAreZero(header)) {
      if (offset + 2U * kUstarBlockBytes != archive.size() ||
          !UstarBytesAreZero(archive.substr(offset + kUstarBlockBytes, kUstarBlockBytes))) {
        throw spio::ValidationError("source archive must end with exactly two zero blocks and no trailing bytes");
      }
      trailer_seen = true;
      offset += 2U * kUstarBlockBytes;
      break;
    }

    uint64_t computed_checksum = 0U;
    for (size_t index = 0U; index < header.size(); ++index) {
      computed_checksum +=
        index >= 148U && index < 156U
          ? static_cast<uint64_t>(' ')
          : static_cast<uint64_t>(static_cast<unsigned char>(header[index]));
    }
    if (ParseCanonicalUstarChecksum(header.substr(148U, 8U)) != computed_checksum) {
      throw spio::ValidationError("source archive header checksum mismatch");
    }
    if (ParseCanonicalUstarOctal(header.substr(100U, 8U), "mode") != 0644U ||
        ParseCanonicalUstarOctal(header.substr(108U, 8U), "uid") != 0U ||
        ParseCanonicalUstarOctal(header.substr(116U, 8U), "gid") != 0U ||
        ParseCanonicalUstarOctal(header.substr(136U, 12U), "mtime") != 0U) {
      throw spio::ValidationError("source archive members must use mode 0644 and zero uid, gid, and mtime");
    }
    if (header[156] != '0') {
      throw spio::ValidationError("source archive may contain regular files only");
    }
    if (header.substr(257U, 6U) != std::string_view("ustar\0", 6U) ||
        header.substr(263U, 2U) != "00") {
      throw spio::ValidationError("source archive must use POSIX ustar magic and version");
    }
    if (!UstarBytesAreZero(header.substr(157U, 100U)) ||
        !UstarBytesAreZero(header.substr(265U, 80U)) ||
        !UstarBytesAreZero(header.substr(500U, 12U))) {
      throw spio::ValidationError("source archive link, owner, device, and extension fields must be empty");
    }

    const std::string name = ParseCanonicalUstarText(header.substr(0U, 100U), "name");
    const std::string prefix = ParseCanonicalUstarText(header.substr(345U, 155U), "prefix");
    const std::string path = prefix.empty() ? name : prefix + "/" + name;
    const std::vector<std::string_view> path_parts = ValidateCanonicalUstarPath(path);
    if (!paths.insert(path).second) {
      throw spio::ValidationError("source archive contains a duplicate member path");
    }
    if (previous_path.has_value() && *previous_path >= path) {
      throw spio::ValidationError("source archive member paths must be strictly increasing");
    }
    previous_path = path;
    if (!top_level_prefix.has_value()) {
      top_level_prefix = std::string(path_parts.front());
    }
    else if (*top_level_prefix != path_parts.front()) {
      throw spio::ValidationError("source archive members must share one top-level package prefix");
    }

    const uint64_t file_size = ParseCanonicalUstarOctal(header.substr(124U, 12U), "size");
    const size_t data_offset = offset + kUstarBlockBytes;
    if (file_size > static_cast<uint64_t>(archive.size() - data_offset)) {
      throw spio::ValidationError("source archive member size exceeds archive bounds");
    }
    const size_t file_size_native = static_cast<size_t>(file_size);
    const size_t padded_size =
      ((file_size_native + kUstarBlockBytes - 1U) / kUstarBlockBytes) * kUstarBlockBytes;
    if (padded_size > archive.size() - data_offset) {
      throw spio::ValidationError("source archive member padding exceeds archive bounds");
    }
    if (!UstarBytesAreZero(
          archive.substr(data_offset + file_size_native, padded_size - file_size_native)
        )) {
      throw spio::ValidationError("source archive member data padding must be zero");
    }

    if (path_parts.back() == "pafio.toml") {
      ++manifest_count;
      if (path_parts.size() != 2U || file_size > kMaxPafioManifestBytes) {
        throw spio::ValidationError(
          "source archive manifest must be exactly <prefix>/pafio.toml and no larger than 1048576 bytes"
        );
      }
      manifest_bytes.assign(archive.substr(data_offset, file_size_native));
    }
    offset = data_offset + padded_size;
  }

  if (!trailer_seen || offset != archive.size()) {
    throw spio::ValidationError("source archive must end with exactly two zero blocks");
  }
  if (manifest_count != 1U) {
    throw spio::ValidationError("source archive must contain exactly one <prefix>/pafio.toml");
  }
  return manifest_bytes;
}

const toml::table &
RequirePafioManifestTable(
  const toml::table &parent,
  std::string_view field,
  const std::string &context
) {
  const toml::table *table = parent[field].as_table();
  if (table == nullptr) {
    throw spio::ValidationError("archived pafio.toml is missing or has invalid [" +
                                std::string(field) + "] in " + context);
  }
  return *table;
}

std::string
RequirePafioManifestString(
  const toml::table &table,
  std::string_view field,
  const std::string &context
) {
  const std::optional<std::string> value = table[field].value<std::string>();
  if (!value.has_value() || value->empty()) {
    throw spio::ValidationError("archived pafio.toml " + context + "." +
                                std::string(field) + " must be a non-empty string");
  }
  return *value;
}

bool
IsStrictPafioVersion(std::string_view value) {
  size_t dots = 0U;
  size_t component_digits = 0U;
  for (const unsigned char byte : value) {
    if (byte == '.') {
      if (component_digits == 0U || dots == 2U) {
        return false;
      }
      ++dots;
      component_digits = 0U;
      continue;
    }
    if (byte < '0' || byte > '9') {
      return false;
    }
    ++component_digits;
  }
  return dots == 2U && component_digits > 0U;
}

nlohmann::json
ParsePafioManifestDependencies(
  const toml::table &document,
  std::string_view table_name
) {
  if (!document.contains(table_name)) {
    return nlohmann::json::array();
  }
  const toml::table *dependencies = document[table_name].as_table();
  if (dependencies == nullptr) {
    throw spio::ValidationError("archived pafio.toml [" + std::string(table_name) +
                                "] must be a table");
  }

  static const std::set<std::string_view> allowed_fields = {
    "package",
    "registry",
    "version",
  };
  std::vector<nlohmann::json> normalized;
  normalized.reserve(dependencies->size());
  for (const auto &[alias_key, node] : *dependencies) {
    const std::string alias(alias_key.str());
    const toml::table *dependency = node.as_table();
    if (dependency == nullptr || !dependency->is_inline()) {
      throw spio::ValidationError("dependency '" + alias + "' in archived pafio.toml [" +
                                  std::string(table_name) + "] must be an inline table");
    }
    if (dependency->size() != allowed_fields.size()) {
      throw spio::ValidationError("dependency '" + alias + "' in archived pafio.toml [" +
                                  std::string(table_name) +
                                  "] must contain exactly package, version, and registry");
    }
    for (const auto &[field_key, ignored] : *dependency) {
      (void) ignored;
      if (!allowed_fields.contains(field_key.str())) {
        throw spio::ValidationError("dependency '" + alias + "' in archived pafio.toml [" +
                                    std::string(table_name) + "] contains unsupported field '" +
                                    std::string(field_key.str()) + "'");
      }
    }
    const std::string version =
      RequirePafioManifestString(*dependency, "version", "dependency '" + alias + "'");
    if (!IsStrictPafioVersion(version)) {
      throw spio::ValidationError("dependency '" + alias + "' in archived pafio.toml [" +
                                  std::string(table_name) + "] version must be strict x.y.z");
    }
    normalized.push_back({
      {"alias", alias},
      {"package", RequirePafioManifestString(*dependency, "package", "dependency '" + alias + "'")},
      {"version_req", version},
      {"registry", RequirePafioManifestString(*dependency, "registry", "dependency '" + alias + "'")},
    });
  }
  std::stable_sort(
    normalized.begin(),
    normalized.end(),
    [](const nlohmann::json &left, const nlohmann::json &right)
    {
      return left.at("alias").get_ref<const std::string &>() <
             right.at("alias").get_ref<const std::string &>();
    }
  );
  nlohmann::json result = nlohmann::json::array();
  for (nlohmann::json &dependency : normalized) {
    result.push_back(std::move(dependency));
  }
  return result;
}

void
ValidatePafioManifest(
  std::string_view manifest_bytes,
  const PublishDraft &draft
) {
  toml::table document;
  try {
    document = toml::parse(manifest_bytes, std::string_view("pafio.toml"));
  }
  catch (const toml::parse_error &) {
    throw spio::ValidationError("archived pafio.toml must be valid UTF-8 TOML");
  }
  if (document.contains("spio")) {
    throw spio::ValidationError("archived pafio.toml must not contain [spio]");
  }
  const toml::table &pafio = RequirePafioManifestTable(document, "pafio", "manifest");
  const std::optional<int64_t> manifest_version = pafio["manifest-version"].value<int64_t>();
  if (!manifest_version.has_value() || *manifest_version != 1) {
    throw spio::ValidationError("archived pafio.toml [pafio].manifest-version must be 1");
  }
  const toml::table &package = RequirePafioManifestTable(document, "package", "manifest");
  if (RequirePafioManifestString(package, "name", "package") != draft.package) {
    throw spio::ValidationError("request package does not match archived pafio.toml");
  }
  const std::string package_version = RequirePafioManifestString(package, "version", "package");
  if (!IsStrictPafioVersion(package_version)) {
    throw spio::ValidationError("archived pafio.toml package.version must be strict x.y.z");
  }
  if (package_version != draft.version) {
    throw spio::ValidationError("request version does not match archived pafio.toml");
  }
  const std::optional<bool> publish = package["publish"].value<bool>();
  if (!publish.has_value() || !*publish) {
    throw spio::ValidationError("archived pafio.toml package.publish must be true");
  }
  if (ParsePafioManifestDependencies(document, "dependencies") != draft.dependencies) {
    throw spio::ValidationError("request dependencies do not match archived pafio.toml");
  }
  if (ParsePafioManifestDependencies(document, "dev-dependencies") != draft.dev_dependencies) {
    throw spio::ValidationError("request dev_dependencies do not match archived pafio.toml");
  }
}

nlohmann::json
ValidatePublishDependencies(const nlohmann::json &body, const std::string &field) {
  if (!body.contains(field) || !body.at(field).is_array()) {
    throw spio::ValidationError(field + " must be an array");
  }
  if (body.at(field).size() > kMaxPublishDependenciesPerTable) {
    throw spio::ValidationError(field + " exceeds the 256-entry limit");
  }

  static const std::set<std::string> dependency_fields = {
    "alias",
    "package",
    "registry",
    "version_req",
  };
  std::vector<nlohmann::json> ordered;
  ordered.reserve(body.at(field).size());
  std::set<std::string> aliases;
  for (const nlohmann::json &dependency : body.at(field)) {
    ValidateExactPublishFields(dependency, dependency_fields, field + " item");
    ValidatePublishString(dependency, "alias", kMaxPublishAliasBytes);
    ValidatePublishString(dependency, "package", kMaxPublishPackageBytes);
    ValidatePublishString(dependency, "version_req", kMaxPublishVersionBytes);
    ValidatePublishString(dependency, "registry", kMaxPublishRegistryBytes);
    const std::string package = dependency.at("package").get<std::string>();
    if (const std::optional<std::string> error = ValidatePackageName(package); error.has_value()) {
      throw spio::ValidationError(field + " package is invalid: " + *error);
    }
    const std::string alias = dependency.at("alias").get<std::string>();
    if (!IsStrictPafioVersion(dependency.at("version_req").get_ref<const std::string &>())) {
      throw spio::ValidationError(field + " version_req must be strict x.y.z");
    }
    if (!aliases.insert(alias).second) {
      throw spio::ValidationError(field + " contains duplicate alias: " + alias);
    }
    ordered.push_back(dependency);
  }
  std::stable_sort(
    ordered.begin(),
    ordered.end(),
    [](const nlohmann::json &left, const nlohmann::json &right)
    {
      return left.at("alias").get_ref<const std::string &>() <
             right.at("alias").get_ref<const std::string &>();
    }
  );
  nlohmann::json result = nlohmann::json::array();
  for (nlohmann::json &dependency : ordered) {
    result.push_back(std::move(dependency));
  }
  return result;
}

PublishDraft
BuildPublishDraft(const nlohmann::json &body, const std::string &publisher_actor) {
  static const std::set<std::string> publish_fields = {
    "archive_base64",
    "archive_name",
    "archive_sha256",
    "archive_size_bytes",
    "dependencies",
    "dev_dependencies",
    "package",
    "publisher_id",
    "version",
  };
  ValidateExactPublishFields(body, publish_fields, "publish request");
  ValidatePublishString(body, "package", kMaxPublishPackageBytes);
  ValidatePublishString(body, "version", kMaxPublishVersionBytes);
  ValidatePublishString(body, "archive_name", kMaxPublishArchiveNameBytes);
  ValidatePublishString(body, "archive_base64", kMaxPublishArchiveBase64Bytes);
  ValidatePublishString(body, "archive_sha256", 64U);
  ValidatePublishString(body, "publisher_id", kMaxPublishPublisherBytes);
  PublishDraft draft;
  draft.package = body.at("package").get<std::string>();
  draft.version = body.at("version").get<std::string>();
  draft.publisher_id = publisher_actor;
  draft.archive_name = body.at("archive_name").get<std::string>();
  draft.archive_sha256 = body.at("archive_sha256").get<std::string>();
  if (const std::optional<std::string> error = ValidatePackageName(draft.package); error.has_value()) {
    throw spio::ValidationError(*error);
  }
  if (!IsStrictPafioVersion(draft.version)) {
    throw spio::ValidationError("version must be strict x.y.z");
  }
  if (!IsSafePublishArchiveName(draft.archive_name)) {
    throw spio::ValidationError("archive_name must be a safe basename ending in .pafio.src.tar");
  }
  if (!IsLowerHexDigest(draft.archive_sha256)) {
    throw spio::ValidationError("archive_sha256 must be a lowercase SHA-256 digest");
  }
  if (!body.at("archive_size_bytes").is_number_unsigned() &&
      !body.at("archive_size_bytes").is_number_integer()) {
    throw spio::ValidationError("archive_size_bytes must be an integer");
  }
  if (body.at("archive_size_bytes").is_number_integer() &&
      body.at("archive_size_bytes").get<int64_t>() <= 0) {
    throw spio::ValidationError("archive_size_bytes must be greater than zero");
  }
  try {
    draft.archive_size_bytes = body.at("archive_size_bytes").get<uint64_t>();
  }
  catch (const std::exception &) {
    throw spio::ValidationError("archive_size_bytes must be a positive integer");
  }
  if (draft.archive_size_bytes == 0U || draft.archive_size_bytes > kMaxPublishArchiveBytes) {
    throw spio::ValidationError("archive_size_bytes must be between 1 and 67108864");
  }
  const std::string &archive_base64 = body.at("archive_base64").get_ref<const std::string &>();
  if (ValidateCanonicalPublishBase64(archive_base64) != draft.archive_size_bytes) {
    throw spio::ValidationError("archive_base64 decoded size does not match archive_size_bytes");
  }
  draft.dependencies = ValidatePublishDependencies(body, "dependencies");
  draft.dev_dependencies = ValidatePublishDependencies(body, "dev_dependencies");
  return draft;
}

std::string
PublishSecureRandomHex(const size_t bytes) {
  std::vector<unsigned char> random(bytes);
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    throw std::runtime_error("secure random generation failed");
  }
  return HexBytes(random.data(), random.size());
}

void
MaterializePublishArchive(
  const nlohmann::json &body,
  const PlatformConfig &config,
  PublishDraft &draft
) {
  const std::string &archive_base64 = body.at("archive_base64").get_ref<const std::string &>();
  const std::string decoded = DecodeCanonicalPublishBase64(archive_base64, draft.archive_size_bytes);
  if (Sha256Bytes(decoded) != draft.archive_sha256) {
    throw spio::ValidationError("decoded archive SHA-256 does not match archive_sha256");
  }
  const std::string manifest_bytes = ValidateDeterministicPafioUstar(decoded);
  ValidatePafioManifest(manifest_bytes, draft);
  draft.manifest_digest = Sha256Bytes(manifest_bytes);

  const fs::path registry_root(config.registry.root);
  draft.remove_empty_registry_root_on_cleanup = !fs::exists(registry_root);
  draft.staging_root = registry_root / "_staging" / "uploads";
  fs::create_directories(draft.staging_root);
  for (size_t attempt = 0; attempt < 16U; ++attempt) {
    const fs::path candidate = draft.staging_root / ("publish-" + PublishSecureRandomHex(16U));
    std::error_code create_error;
    if (fs::create_directory(candidate, create_error)) {
      draft.staging_directory = candidate;
      break;
    }
    if (create_error) {
      throw std::runtime_error("failed to create private publish staging directory: " + create_error.message());
    }
  }
  if (draft.staging_directory.empty()) {
    throw std::runtime_error("failed to allocate unique publish staging directory");
  }

  std::error_code permission_error;
  fs::permissions(draft.staging_directory, fs::perms::owner_all, fs::perm_options::replace, permission_error);
  if (permission_error) {
    throw std::runtime_error("failed to secure publish staging directory: " + permission_error.message());
  }
  draft.archive_path = draft.staging_directory / "upload.pafio.src.tar";
  {
    std::ofstream output(draft.archive_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("failed to open publish staging archive");
    }
    output.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
    output.flush();
    if (!output) {
      throw std::runtime_error("failed to write complete publish staging archive");
    }
  }
  fs::permissions(
    draft.archive_path,
    fs::perms::owner_read | fs::perms::owner_write,
    fs::perm_options::replace,
    permission_error
  );
  if (permission_error) {
    throw std::runtime_error("failed to secure publish staging archive: " + permission_error.message());
  }
}

nlohmann::json
ReleaseDependencies(const nlohmann::json &dependencies, const std::string &kind) {
  nlohmann::json result = nlohmann::json::array();
  for (const nlohmann::json &dependency : dependencies) {
    nlohmann::json release_dependency = dependency;
    release_dependency["kind"] = kind;
    release_dependency["optional"] = false;
    release_dependency["target_condition"] = "";
    release_dependency["features"] = nlohmann::json::array();
    result.push_back(std::move(release_dependency));
  }
  return result;
}

nlohmann::json
BuildReleaseRecord(
  const PublishDraft &draft,
  const std::string &published_at,
  const std::string &archive_sha256,
  const uintmax_t archive_size,
  const std::string &artifact_path
) {
  const nlohmann::json dependencies = ReleaseDependencies(draft.dependencies, "runtime");
  const nlohmann::json dev_dependencies = ReleaseDependencies(draft.dev_dependencies, "development");
  const nlohmann::json metadata_source = {
    {"package", draft.package},
    {"version", draft.version},
    {"publisher_id", draft.publisher_id},
    {"published_at", published_at},
    {"archive_sha256", archive_sha256},
    {"dependencies", dependencies},
    {"dev_dependencies", dev_dependencies},
  };
  return {
    {"schema_version", 1},
    {"package", draft.package},
    {"version", draft.version},
    {"release_revision", 1},
    {"published_at", published_at},
    {"publisher_id", draft.publisher_id},
    {"yanked", false},
    {"deprecated_message", ""},
    {"source_artifact", {
                          {"sha256", archive_sha256},
                          {"size_bytes", static_cast<int64_t>(archive_size)},
                          {"path", artifact_path},
                          {"archive_format", "tar"},
                          {"compression", "none"},
                        }},
    {"binary_artifacts", nlohmann::json::array()},
    {"dependencies", dependencies},
    {"dev_dependencies", dev_dependencies},
    {"features", {
                   {"default", nlohmann::json::array()},
                   {"optional", nlohmann::json::array()},
                 }},
    {"manifest_digest", draft.manifest_digest},
    {"metadata_digest", Sha256Bytes(CanonicalJson(metadata_source))},
  };
}

struct LocalAppendResult
{
  std::string index_path;
  std::string log_leaf_path;
  size_t sequence = 0;
};

LocalAppendResult
AppendRegistryReleaseToLocal(
  const PlatformConfig &config,
  const PublishDraft &draft,
  const nlohmann::json &release_record,
  const std::string &artifact_path
) {
  const fs::path registry_root(config.registry.root);
  const fs::path artifact_dest_path = registry_root / artifact_path;
  fs::create_directories(artifact_dest_path.parent_path());
  if (fs::exists(artifact_dest_path)) {
    if (spio::Sha256File(artifact_dest_path) != release_record.at("source_artifact").at("sha256").get<std::string>()) {
      throw std::runtime_error("destination artifact already exists with different content");
    }
  }
  else {
    fs::copy_file(draft.archive_path, artifact_dest_path);
  }

  const std::string index_path = RegistryIndexPathForPackage(draft.package);
  const fs::path index_file = registry_root / index_path;
  if (fs::exists(index_file)) {
    std::ifstream in(index_file);
    std::string line;
    while (std::getline(in, line)) {
      if (JsonLineHasRelease(line, draft.package, draft.version)) {
        throw std::runtime_error("package version is already published");
      }
    }
  }
  AppendJsonLine(index_file, release_record);

  const size_t sequence = LeafSequencePaths(registry_root).size() + 1U;
  const std::string log_leaf_path = "log/leaves/" + PaddedNumber(sequence, 12) + ".json";
  const std::string package_namespace = SplitPackageName(draft.package).front();
  const nlohmann::json leaf = {
    {"schema_version", 1},
    {"sequence", static_cast<int64_t>(sequence)},
    {"namespace", package_namespace},
    {"package", draft.package},
    {"version", draft.version},
    {"release_revision", 1},
    {"index_path", index_path},
    {"index_record_sha256", Sha256Bytes(CanonicalJson(release_record))},
    {"source_artifact_sha256", release_record.at("source_artifact").at("sha256").get<std::string>()},
    {"source_artifact_path", artifact_path},
  };
  WriteTextFile(registry_root / log_leaf_path, JsonText(leaf));
  return {.index_path = index_path, .log_leaf_path = log_leaf_path, .sequence = sequence};
}

}  // namespace

}  // namespace spio::platform
