#pragma once

#include <filesystem>
#include <string>

namespace spio::platform
{

struct PlatformCertificateAuthorityConfig
{
  std::filesystem::path root_dir = ".styio-platform/mtls";
  std::string spiffe_trust_domain = "styio-platform";
  int key_bits = 2048;
  int ca_valid_days = 3650;
  int leaf_valid_days = 397;
};

struct PlatformCertificateSubject
{
  std::string role;
  std::string tenant_id;
  std::string node_id;
  std::string common_name;
};

struct PlatformCertificateBundle
{
  std::filesystem::path ca_certificate_path;
  std::filesystem::path ca_private_key_path;
  std::filesystem::path certificate_path;
  std::filesystem::path private_key_path;
  std::string identity_uri_san;
};

PlatformCertificateSubject BuildPlatformNodeCertificateSubject(
    std::string role,
    std::string tenant_id,
    std::string node_id);

std::string BuildPlatformMtlsUriSan(
    const PlatformCertificateAuthorityConfig &config,
    const PlatformCertificateSubject &subject);

PlatformCertificateBundle EnsurePlatformCertificateAuthority(
    const PlatformCertificateAuthorityConfig &config);

PlatformCertificateBundle EnsurePlatformMtlsCertificate(
    const PlatformCertificateAuthorityConfig &config,
    const PlatformCertificateSubject &subject);

}  // namespace spio::platform
