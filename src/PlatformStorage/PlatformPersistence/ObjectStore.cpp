#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"

#include "PlatformCore/Core/Process.hpp"
#include "PlatformCore/Core/Sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

std::string SanitizeKeyPart(std::string_view value)
{
  std::string result;
  result.reserve(value.size());
  for (const char ch : value)
  {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
    {
      result.push_back(ch);
    }
    else
    {
      result.push_back('_');
    }
  }
  if (result.empty())
  {
    return "unset";
  }
  return result;
}

std::string Hex(const unsigned char *data, const size_t size)
{
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t index = 0; index < size; ++index)
  {
    out << std::setw(2) << static_cast<int>(data[index]);
  }
  return out.str();
}

std::string Sha256Hex(std::string_view value)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest);
  return Hex(digest, SHA256_DIGEST_LENGTH);
}

std::vector<unsigned char> HmacSha256(const std::vector<unsigned char> &key, std::string_view message)
{
  unsigned int length = 0;
  unsigned char digest[EVP_MAX_MD_SIZE];
  HMAC(
      EVP_sha256(),
      key.data(),
      static_cast<int>(key.size()),
      reinterpret_cast<const unsigned char *>(message.data()),
      message.size(),
      digest,
      &length);
  return {digest, digest + length};
}

std::vector<unsigned char> HmacSha256(std::string_view key, std::string_view message)
{
  return HmacSha256(
      std::vector<unsigned char>(reinterpret_cast<const unsigned char *>(key.data()), reinterpret_cast<const unsigned char *>(key.data() + key.size())),
      message);
}

std::string UrlEncode(std::string_view value, const bool encode_slash)
{
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setfill('0');
  for (const unsigned char ch : value)
  {
    const bool safe = std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || (!encode_slash && ch == '/');
    if (safe)
    {
      out << static_cast<char>(ch);
    }
    else
    {
      out << "%" << std::setw(2) << static_cast<int>(ch);
    }
  }
  return out.str();
}

std::string TrimSlashes(std::string value)
{
  while (!value.empty() && value.front() == '/')
  {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == '/')
  {
    value.pop_back();
  }
  return value;
}

std::string ApplyPrefix(const ObjectStoreConfig &config, std::string key)
{
  const std::string prefix = TrimSlashes(config.prefix);
  if (prefix.empty())
  {
    return key;
  }
  return prefix + "/" + key;
}

std::string StripPrefix(const ObjectStoreConfig &config, std::string key)
{
  const std::string prefix = TrimSlashes(config.prefix);
  if (prefix.empty())
  {
    return key;
  }
  const std::string marker = prefix + "/";
  if (key == prefix)
  {
    return "";
  }
  if (key.starts_with(marker))
  {
    return key.substr(marker.size());
  }
  return key;
}

std::string NormalizeObjectPrefix(std::string_view prefix)
{
  std::string value(prefix);
  while (!value.empty() && value.front() == '/')
  {
    value.erase(value.begin());
  }
  if (value.empty())
  {
    return value;
  }
  if (value.find('\\') != std::string::npos)
  {
    throw std::runtime_error("object prefix must be a POSIX relative path");
  }
  std::stringstream stream(value);
  std::string part;
  while (std::getline(stream, part, '/'))
  {
    if (part.empty() || part == "." || part == "..")
    {
      throw std::runtime_error("object prefix must be canonical and stay inside the object root");
    }
  }
  return value;
}

struct S3Endpoint
{
  std::string scheme;
  std::string host;
  std::string base_path;
};

S3Endpoint ParseEndpoint(const ObjectStoreConfig &config)
{
  if (config.endpoint.empty())
  {
    throw std::runtime_error("S3 object store requires STYIO_PLATFORM_OBJECT_STORE_ENDPOINT");
  }
  const size_t scheme_end = config.endpoint.find("://");
  if (scheme_end == std::string::npos)
  {
    throw std::runtime_error("S3 endpoint must include http:// or https://");
  }
  S3Endpoint endpoint;
  endpoint.scheme = config.endpoint.substr(0, scheme_end);
  std::string rest = config.endpoint.substr(scheme_end + 3);
  const size_t slash = rest.find('/');
  endpoint.host = slash == std::string::npos ? rest : rest.substr(0, slash);
  endpoint.base_path = slash == std::string::npos ? "" : TrimSlashes(rest.substr(slash + 1));
  if (endpoint.host.empty())
  {
    throw std::runtime_error("S3 endpoint host is empty");
  }
  return endpoint;
}

std::string S3CanonicalPath(const ObjectStoreConfig &config, const std::string &key)
{
  const S3Endpoint endpoint = ParseEndpoint(config);
  std::vector<std::string> parts;
  if (!endpoint.base_path.empty())
  {
    parts.push_back(endpoint.base_path);
  }
  if (config.path_style)
  {
    parts.push_back(config.bucket);
  }
  parts.push_back(key);
  std::ostringstream path;
  path << "/";
  for (size_t index = 0; index < parts.size(); ++index)
  {
    if (index > 0)
    {
      path << "/";
    }
    path << UrlEncode(parts[index], false);
  }
  return path.str();
}

std::string S3Url(const ObjectStoreConfig &config, const std::string &key, const std::string &query = "")
{
  const S3Endpoint endpoint = ParseEndpoint(config);
  const std::string host = config.path_style ? endpoint.host : config.bucket + "." + endpoint.host;
  const std::string url = endpoint.scheme + "://" + host + S3CanonicalPath(config, key);
  return query.empty() ? url : url + "?" + query;
}

std::string AmzTimestamp()
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
  return out.str();
}

std::string DateFromAmzTimestamp(const std::string &timestamp)
{
  return timestamp.substr(0, 8);
}

std::vector<unsigned char> S3SigningKey(const ObjectStoreConfig &config, const std::string &date)
{
  std::vector<unsigned char> k_date = HmacSha256("AWS4" + config.secret_access_key, date);
  std::vector<unsigned char> k_region = HmacSha256(k_date, config.region);
  std::vector<unsigned char> k_service = HmacSha256(k_region, "s3");
  return HmacSha256(k_service, "aws4_request");
}

std::vector<std::string> S3SignedHeaders(
    const ObjectStoreConfig &config,
    const std::string &method,
    const std::string &key,
    const std::string &payload_hash,
    const std::string &query = "")
{
  if (config.bucket.empty() || config.access_key_id.empty() || config.secret_access_key.empty())
  {
    throw std::runtime_error("S3 object store requires bucket, access key id, and secret access key");
  }
  const S3Endpoint endpoint = ParseEndpoint(config);
  const std::string host = config.path_style ? endpoint.host : config.bucket + "." + endpoint.host;
  const std::string timestamp = AmzTimestamp();
  const std::string date = DateFromAmzTimestamp(timestamp);
  std::string canonical_headers =
      "host:" + host + "\n" +
      "x-amz-content-sha256:" + payload_hash + "\n" +
      "x-amz-date:" + timestamp + "\n";
  std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  if (!config.session_token.empty())
  {
    canonical_headers += "x-amz-security-token:" + config.session_token + "\n";
    signed_headers += ";x-amz-security-token";
  }
  const std::string canonical_request =
      method + "\n" +
      S3CanonicalPath(config, key) + "\n" +
      query + "\n" +
      canonical_headers + "\n" +
      signed_headers + "\n" +
      payload_hash;
  const std::string scope = date + "/" + config.region + "/s3/aws4_request";
  const std::string string_to_sign =
      "AWS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" + Sha256Hex(canonical_request);
  const std::vector<unsigned char> signing_key = S3SigningKey(config, date);
  const std::vector<unsigned char> signature_bytes = HmacSha256(signing_key, string_to_sign);
  const std::string signature = Hex(signature_bytes.data(), signature_bytes.size());
  std::vector<std::string> headers = {
      "Host: " + host,
      "x-amz-content-sha256: " + payload_hash,
      "x-amz-date: " + timestamp,
      "Authorization: AWS4-HMAC-SHA256 Credential=" + config.access_key_id + "/" + scope +
          ", SignedHeaders=" + signed_headers + ", Signature=" + signature,
  };
  if (!config.session_token.empty())
  {
    headers.push_back("x-amz-security-token: " + config.session_token);
  }
  return headers;
}

spio::ProcessResult RunCurl(std::vector<std::string> args, std::string stdin_text = {})
{
  spio::ProcessRequest request{
      .program = "curl",
      .args = std::move(args),
      .timeout = std::chrono::seconds{60},
      .max_stdout_bytes = 16U << 20,
      .max_stderr_bytes = 1U << 20,
      .stdin_text = std::move(stdin_text),
      .error_context = "curl s3 object request",
  };
  spio::ProcessResult result = spio::RunProcess(request);
  if (result.exit_code != 0)
  {
    throw std::runtime_error("curl S3 request failed: " + spio::DescribeProcessFailure(result));
  }
  return result;
}

void AppendHeaders(std::vector<std::string> &args, const std::vector<std::string> &headers)
{
  for (const std::string &header : headers)
  {
    args.push_back("-H");
    args.push_back(header);
  }
}

std::vector<std::string> ExtractXmlKeys(const std::string &xml)
{
  std::vector<std::string> keys;
  size_t offset = 0;
  while (true)
  {
    const size_t begin = xml.find("<Key>", offset);
    if (begin == std::string::npos)
    {
      break;
    }
    const size_t end = xml.find("</Key>", begin);
    if (end == std::string::npos)
    {
      break;
    }
    keys.push_back(xml.substr(begin + 5, end - begin - 5));
    offset = end + 6;
  }
  return keys;
}

}  // namespace

ObjectStoreProvider ParseObjectStoreProvider(std::string_view value)
{
  if (value == "gcs")
  {
    return ObjectStoreProvider::Gcs;
  }
  if (value == "azure")
  {
    return ObjectStoreProvider::Azure;
  }
  if (value == "filesystem")
  {
    return ObjectStoreProvider::Filesystem;
  }
  if (value == "memory")
  {
    return ObjectStoreProvider::Memory;
  }
  return ObjectStoreProvider::S3;
}

std::string ToString(ObjectStoreProvider provider)
{
  switch (provider)
  {
    case ObjectStoreProvider::S3:
      return "s3";
    case ObjectStoreProvider::Gcs:
      return "gcs";
    case ObjectStoreProvider::Azure:
      return "azure";
    case ObjectStoreProvider::Filesystem:
      return "filesystem";
    case ObjectStoreProvider::Memory:
      return "memory";
  }
  return "s3";
}

bool IsObjectStoreProviderImplemented(ObjectStoreProvider provider)
{
  return provider == ObjectStoreProvider::S3 || provider == ObjectStoreProvider::Filesystem || provider == ObjectStoreProvider::Memory;
}

std::string BuildArtifactObjectKey(std::string_view tenant_id, std::string_view workspace_id, std::string_view job_id, std::string_view artifact_name)
{
  return "tenants/" + SanitizeKeyPart(tenant_id) +
         "/workspaces/" + SanitizeKeyPart(workspace_id) +
         "/jobs/" + SanitizeKeyPart(job_id) +
         "/artifacts/" + SanitizeKeyPart(artifact_name);
}

std::string NormalizeObjectKey(std::string_view key)
{
  std::string value(key);
  while (!value.empty() && value.front() == '/')
  {
    value.erase(value.begin());
  }
  if (value.empty() || value.ends_with("/") || value.find('\\') != std::string::npos)
  {
    throw std::runtime_error("object key must be a non-empty POSIX relative path");
  }
  std::stringstream stream(value);
  std::string part;
  while (std::getline(stream, part, '/'))
  {
    if (part.empty() || part == "." || part == "..")
    {
      throw std::runtime_error("object key must be canonical and stay inside the object root");
    }
  }
  return value;
}

void PutObjectBytes(const ObjectStoreConfig &config, std::string_view key, std::string_view payload, std::string_view content_type)
{
  const std::string object_key = ApplyPrefix(config, NormalizeObjectKey(key));
  if (ParseObjectStoreProvider(config.provider) != ObjectStoreProvider::S3)
  {
    throw std::runtime_error("PutObjectBytes currently requires the S3 object store provider");
  }
  const std::string payload_hash = Sha256Hex(payload);
  std::vector<std::string> args = {"-sS", "-X", "PUT", "--upload-file", "-", S3Url(config, object_key)};
  AppendHeaders(args, S3SignedHeaders(config, "PUT", object_key, payload_hash));
  args.push_back("-H");
  args.push_back("Content-Type: " + std::string(content_type));
  RunCurl(std::move(args), std::string(payload));
}

void PutObjectFile(const ObjectStoreConfig &config, std::string_view key, const fs::path &path, std::string_view content_type)
{
  const std::string object_key = ApplyPrefix(config, NormalizeObjectKey(key));
  if (ParseObjectStoreProvider(config.provider) != ObjectStoreProvider::S3)
  {
    throw std::runtime_error("PutObjectFile currently requires the S3 object store provider");
  }
  const std::string payload_hash = spio::Sha256File(path);
  std::vector<std::string> args = {"-sS", "-X", "PUT", "--upload-file", path.string(), S3Url(config, object_key)};
  AppendHeaders(args, S3SignedHeaders(config, "PUT", object_key, payload_hash));
  args.push_back("-H");
  args.push_back("Content-Type: " + std::string(content_type));
  RunCurl(std::move(args));
}

std::optional<std::string> GetObjectText(const ObjectStoreConfig &config, std::string_view key)
{
  const std::string object_key = ApplyPrefix(config, NormalizeObjectKey(key));
  if (ParseObjectStoreProvider(config.provider) != ObjectStoreProvider::S3)
  {
    throw std::runtime_error("GetObjectText currently requires the S3 object store provider");
  }
  std::vector<std::string> args = {"-sS", "-f", S3Url(config, object_key)};
  AppendHeaders(args, S3SignedHeaders(config, "GET", object_key, "UNSIGNED-PAYLOAD"));
  try
  {
    return RunCurl(std::move(args)).stdout_text;
  }
  catch (const std::exception &)
  {
    return std::nullopt;
  }
}

bool ObjectExists(const ObjectStoreConfig &config, std::string_view key)
{
  const std::string object_key = ApplyPrefix(config, NormalizeObjectKey(key));
  if (ParseObjectStoreProvider(config.provider) != ObjectStoreProvider::S3)
  {
    throw std::runtime_error("ObjectExists currently requires the S3 object store provider");
  }
  std::vector<std::string> args = {"-sS", "-o", "/dev/null", "-w", "%{http_code}", "-I", S3Url(config, object_key)};
  AppendHeaders(args, S3SignedHeaders(config, "HEAD", object_key, "UNSIGNED-PAYLOAD"));
  const spio::ProcessResult result = RunCurl(std::move(args));
  return result.stdout_text.starts_with("2");
}

std::vector<std::string> ListObjectKeys(const ObjectStoreConfig &config, std::string_view prefix)
{
  const std::string normalized = NormalizeObjectPrefix(prefix);
  const std::string normalized_prefix = normalized.empty() ? TrimSlashes(config.prefix) : ApplyPrefix(config, normalized);
  const std::string query = "list-type=2&prefix=" + UrlEncode(normalized_prefix, true);
  std::vector<std::string> args = {"-sS", S3Url(config, "", query)};
  AppendHeaders(args, S3SignedHeaders(config, "GET", "", "UNSIGNED-PAYLOAD", query));
  std::vector<std::string> keys = ExtractXmlKeys(RunCurl(std::move(args)).stdout_text);
  for (std::string &key : keys)
  {
    key = StripPrefix(config, std::move(key));
  }
  keys.erase(std::remove_if(keys.begin(), keys.end(), [](const std::string &key) { return key.empty(); }), keys.end());
  return keys;
}

nlohmann::json DescribeObjectStore(const ObjectStoreConfig &config)
{
  const ObjectStoreProvider provider = ParseObjectStoreProvider(config.provider);
  return {
      {"provider", ToString(provider)},
      {"implemented", IsObjectStoreProviderImplemented(provider)},
      {"bucket_configured", !config.bucket.empty()},
      {"endpoint_configured", !config.endpoint.empty()},
      {"access_key_configured", !config.access_key_id.empty()},
      {"secret_configured", !config.secret_access_key.empty()},
      {"prefix_configured", !config.prefix.empty()},
      {"path_style", config.path_style},
      {"region", config.region},
  };
}

}  // namespace spio::platform
