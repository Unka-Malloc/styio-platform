#include "PlatformSecurity/PlatformCA/CertificateAuthority.hpp"

#include "PlatformSecurity/PlatformClientAuth/Identity.hpp"

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

template <typename T, void (*FreeFn)(T *)>
using OpenSslPtr = std::unique_ptr<T, decltype(FreeFn)>;

using EvpPkeyPtr = OpenSslPtr<EVP_PKEY, EVP_PKEY_free>;
using EvpPkeyCtxPtr = OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
using X509Ptr = OpenSslPtr<X509, X509_free>;
using X509ExtensionPtr = OpenSslPtr<X509_EXTENSION, X509_EXTENSION_free>;
using BigNumPtr = OpenSslPtr<BIGNUM, BN_free>;
using Asn1IntegerPtr = OpenSslPtr<ASN1_INTEGER, ASN1_INTEGER_free>;

std::string OpenSslErrorText()
{
  const unsigned long code = ERR_get_error();
  if (code == 0)
  {
    return "unknown OpenSSL error";
  }
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return buffer.data();
}

void RequireOpenSsl(bool ok, std::string_view message)
{
  if (!ok)
  {
    throw std::runtime_error(std::string(message) + ": " + OpenSslErrorText());
  }
}

std::string SanitizePathPart(std::string_view value)
{
  std::string result;
  result.reserve(value.size());
  for (const char ch : value)
  {
    const bool allowed =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.';
    result.push_back(allowed ? ch : '-');
  }
  return result.empty() ? "unknown" : result;
}

std::string RequireSubjectValue(std::string value, std::string_view field)
{
  if (value.empty())
  {
    throw std::invalid_argument(std::string(field) + " is required for platform certificate subject");
  }
  return value;
}

fs::path CaCertificatePath(const PlatformCertificateAuthorityConfig &config)
{
  return config.root_dir / "ca" / "ca.crt";
}

fs::path CaPrivateKeyPath(const PlatformCertificateAuthorityConfig &config)
{
  return config.root_dir / "ca" / "ca.key";
}

fs::path IssuedCertificateDir(
    const PlatformCertificateAuthorityConfig &config,
    const PlatformCertificateSubject &subject)
{
  return config.root_dir / "issued" / SanitizePathPart(subject.tenant_id) / SanitizePathPart(subject.role) /
         SanitizePathPart(subject.node_id);
}

bool ExistingFile(const fs::path &path)
{
  std::error_code error;
  return fs::is_regular_file(path, error);
}

void EnsureParentDirectory(const fs::path &path)
{
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error)
  {
    throw std::runtime_error("failed to create certificate directory " + path.parent_path().string() + ": " + error.message());
  }
}

void RestrictPrivateKeyPermissions(const fs::path &path)
{
  std::error_code error;
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, error);
}

EvpPkeyPtr GeneratePrivateKey(int key_bits)
{
  EvpPkeyCtxPtr context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  RequireOpenSsl(context != nullptr, "failed to create key generation context");
  RequireOpenSsl(EVP_PKEY_keygen_init(context.get()) == 1, "failed to initialize key generation");
  RequireOpenSsl(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), key_bits) == 1, "failed to set RSA key size");
  EVP_PKEY *raw_key = nullptr;
  RequireOpenSsl(EVP_PKEY_keygen(context.get(), &raw_key) == 1, "failed to generate private key");
  return EvpPkeyPtr(raw_key, EVP_PKEY_free);
}

void WritePrivateKey(const fs::path &path, EVP_PKEY *key)
{
  EnsureParentDirectory(path);
  std::FILE *handle = std::fopen(path.string().c_str(), "wb");
  if (handle == nullptr)
  {
    throw std::runtime_error("failed to open private key for writing: " + path.string());
  }
  const int written = PEM_write_PrivateKey(handle, key, nullptr, nullptr, 0, nullptr, nullptr);
  std::fclose(handle);
  RequireOpenSsl(written == 1, "failed to write private key");
  RestrictPrivateKeyPermissions(path);
}

void WriteCertificate(const fs::path &path, X509 *certificate)
{
  EnsureParentDirectory(path);
  std::FILE *handle = std::fopen(path.string().c_str(), "wb");
  if (handle == nullptr)
  {
    throw std::runtime_error("failed to open certificate for writing: " + path.string());
  }
  const int written = PEM_write_X509(handle, certificate);
  std::fclose(handle);
  RequireOpenSsl(written == 1, "failed to write certificate");
}

EvpPkeyPtr LoadPrivateKey(const fs::path &path)
{
  std::FILE *handle = std::fopen(path.string().c_str(), "rb");
  if (handle == nullptr)
  {
    throw std::runtime_error("failed to open private key: " + path.string());
  }
  EVP_PKEY *key = PEM_read_PrivateKey(handle, nullptr, nullptr, nullptr);
  std::fclose(handle);
  RequireOpenSsl(key != nullptr, "failed to read private key");
  return EvpPkeyPtr(key, EVP_PKEY_free);
}

X509Ptr LoadCertificate(const fs::path &path)
{
  std::FILE *handle = std::fopen(path.string().c_str(), "rb");
  if (handle == nullptr)
  {
    throw std::runtime_error("failed to open certificate: " + path.string());
  }
  X509 *certificate = PEM_read_X509(handle, nullptr, nullptr, nullptr);
  std::fclose(handle);
  RequireOpenSsl(certificate != nullptr, "failed to read certificate");
  return X509Ptr(certificate, X509_free);
}

void SetRandomSerial(X509 *certificate)
{
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
  {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    for (size_t index = 0; index < bytes.size(); ++index)
    {
      bytes[index] = static_cast<unsigned char>((now >> ((index % 8) * 8)) & 0xff);
    }
  }
  bytes[0] &= 0x7f;
  BigNumPtr serial_bn(BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), nullptr), BN_free);
  RequireOpenSsl(serial_bn != nullptr, "failed to create certificate serial");
  Asn1IntegerPtr serial(BN_to_ASN1_INTEGER(serial_bn.get(), nullptr), ASN1_INTEGER_free);
  RequireOpenSsl(serial != nullptr, "failed to convert certificate serial");
  RequireOpenSsl(X509_set_serialNumber(certificate, serial.get()) == 1, "failed to set certificate serial");
}

void SetSubjectName(X509 *certificate, const std::string &common_name)
{
  X509_NAME *name = X509_get_subject_name(certificate);
  RequireOpenSsl(name != nullptr, "failed to access certificate subject");
  RequireOpenSsl(
      X509_NAME_add_entry_by_txt(
          name,
          "CN",
          MBSTRING_ASC,
          reinterpret_cast<const unsigned char *>(common_name.c_str()),
          -1,
          -1,
          0) == 1,
      "failed to set certificate common name");
}

void AddExtension(X509 *certificate, X509 *issuer, int nid, const std::string &value)
{
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509ExtensionPtr extension(
      X509V3_EXT_nconf_nid(nullptr, &context, nid, value.c_str()),
      X509_EXTENSION_free);
  RequireOpenSsl(extension != nullptr, "failed to create certificate extension");
  RequireOpenSsl(X509_add_ext(certificate, extension.get(), -1) == 1, "failed to add certificate extension");
}

X509Ptr CreateCaCertificate(
    EVP_PKEY *private_key,
    const PlatformCertificateAuthorityConfig &config)
{
  X509Ptr certificate(X509_new(), X509_free);
  RequireOpenSsl(certificate != nullptr, "failed to create CA certificate");
  RequireOpenSsl(X509_set_version(certificate.get(), 2) == 1, "failed to set CA certificate version");
  SetRandomSerial(certificate.get());
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), static_cast<long>(config.ca_valid_days) * 24L * 60L * 60L);
  RequireOpenSsl(X509_set_pubkey(certificate.get(), private_key) == 1, "failed to set CA public key");
  SetSubjectName(certificate.get(), "Styio Platform Local CA");
  RequireOpenSsl(
      X509_set_issuer_name(certificate.get(), X509_get_subject_name(certificate.get())) == 1,
      "failed to set CA issuer");
  AddExtension(certificate.get(), certificate.get(), NID_basic_constraints, "critical,CA:TRUE");
  AddExtension(certificate.get(), certificate.get(), NID_key_usage, "critical,keyCertSign,cRLSign");
  AddExtension(certificate.get(), certificate.get(), NID_subject_key_identifier, "hash");
  RequireOpenSsl(X509_sign(certificate.get(), private_key, EVP_sha256()) > 0, "failed to sign CA certificate");
  return certificate;
}

X509Ptr CreateLeafCertificate(
    EVP_PKEY *private_key,
    X509 *ca_certificate,
    EVP_PKEY *ca_private_key,
    const PlatformCertificateAuthorityConfig &config,
    const PlatformCertificateSubject &subject)
{
  X509Ptr certificate(X509_new(), X509_free);
  RequireOpenSsl(certificate != nullptr, "failed to create leaf certificate");
  RequireOpenSsl(X509_set_version(certificate.get(), 2) == 1, "failed to set leaf certificate version");
  SetRandomSerial(certificate.get());
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), static_cast<long>(config.leaf_valid_days) * 24L * 60L * 60L);
  RequireOpenSsl(X509_set_pubkey(certificate.get(), private_key) == 1, "failed to set leaf public key");
  SetSubjectName(certificate.get(), subject.common_name);
  RequireOpenSsl(
      X509_set_issuer_name(certificate.get(), X509_get_subject_name(ca_certificate)) == 1,
      "failed to set leaf issuer");
  AddExtension(certificate.get(), ca_certificate, NID_basic_constraints, "critical,CA:FALSE");
  AddExtension(certificate.get(), ca_certificate, NID_key_usage, "critical,digitalSignature,keyEncipherment");
  AddExtension(certificate.get(), ca_certificate, NID_ext_key_usage, "serverAuth,clientAuth");
  AddExtension(
      certificate.get(),
      ca_certificate,
      NID_subject_alt_name,
      "URI:" + BuildPlatformMtlsUriSan(config, subject));
  RequireOpenSsl(X509_sign(certificate.get(), ca_private_key, EVP_sha256()) > 0, "failed to sign leaf certificate");
  return certificate;
}

PlatformCertificateBundle BundleForSubject(
    const PlatformCertificateAuthorityConfig &config,
    const PlatformCertificateSubject &subject)
{
  const fs::path issued_dir = IssuedCertificateDir(config, subject);
  return {
      .ca_certificate_path = CaCertificatePath(config),
      .ca_private_key_path = CaPrivateKeyPath(config),
      .certificate_path = issued_dir / "tls.crt",
      .private_key_path = issued_dir / "tls.key",
      .identity_uri_san = BuildPlatformMtlsUriSan(config, subject),
  };
}

}  // namespace

PlatformCertificateSubject BuildPlatformNodeCertificateSubject(
    std::string role,
    std::string tenant_id,
    std::string node_id)
{
  role = RequireSubjectValue(std::move(role), "role");
  tenant_id = RequireSubjectValue(std::move(tenant_id), "tenant_id");
  node_id = RequireSubjectValue(std::move(node_id), "node_id");
  if (!IsPlatformServiceRole(role))
  {
    throw std::invalid_argument("unsupported platform certificate role: " + role);
  }
  return {
      .role = role,
      .tenant_id = tenant_id,
      .node_id = node_id,
      .common_name = role + "/" + node_id,
  };
}

std::string BuildPlatformMtlsUriSan(
    const PlatformCertificateAuthorityConfig &config,
    const PlatformCertificateSubject &subject)
{
  return "spiffe://" + config.spiffe_trust_domain + "/tenant/" + subject.tenant_id +
         "/role/" + subject.role + "/node/" + subject.node_id;
}

PlatformCertificateBundle EnsurePlatformCertificateAuthority(
    const PlatformCertificateAuthorityConfig &config)
{
  const fs::path certificate_path = CaCertificatePath(config);
  const fs::path key_path = CaPrivateKeyPath(config);
  if (ExistingFile(certificate_path) && ExistingFile(key_path))
  {
    return {
        .ca_certificate_path = certificate_path,
        .ca_private_key_path = key_path,
        .certificate_path = {},
        .private_key_path = {},
        .identity_uri_san = {},
    };
  }

  EvpPkeyPtr private_key = GeneratePrivateKey(config.key_bits);
  X509Ptr certificate = CreateCaCertificate(private_key.get(), config);
  WritePrivateKey(key_path, private_key.get());
  WriteCertificate(certificate_path, certificate.get());
  return {
      .ca_certificate_path = certificate_path,
      .ca_private_key_path = key_path,
      .certificate_path = {},
      .private_key_path = {},
      .identity_uri_san = {},
  };
}

PlatformCertificateBundle EnsurePlatformMtlsCertificate(
    const PlatformCertificateAuthorityConfig &config,
    const PlatformCertificateSubject &subject)
{
  const PlatformCertificateBundle bundle = BundleForSubject(config, subject);
  EnsurePlatformCertificateAuthority(config);
  if (ExistingFile(bundle.certificate_path) && ExistingFile(bundle.private_key_path))
  {
    return bundle;
  }

  EvpPkeyPtr ca_private_key = LoadPrivateKey(bundle.ca_private_key_path);
  X509Ptr ca_certificate = LoadCertificate(bundle.ca_certificate_path);
  EvpPkeyPtr private_key = GeneratePrivateKey(config.key_bits);
  X509Ptr certificate = CreateLeafCertificate(
      private_key.get(),
      ca_certificate.get(),
      ca_private_key.get(),
      config,
      subject);
  WritePrivateKey(bundle.private_key_path, private_key.get());
  WriteCertificate(bundle.certificate_path, certificate.get());
  return bundle;
}

}  // namespace spio::platform
