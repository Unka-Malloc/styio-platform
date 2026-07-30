#include "BuildTestSupport.hpp"

#include "PlatformCore/Core/Errors.hpp"
#include "PlatformCloud/DeveloperWorkspace/WorkerRuntimeFactory.hpp"
#include "PlatformCloud/PackageRegistry/MirrorSync/MirrorSync.hpp"
#include "PlatformCloud/PackageRegistry/ControlPlane/RegistryRoutes.hpp"
#include "PlatformCloud/PackageRegistry/ReleaseManagement/ReleaseManager.hpp"
#include "PlatformCore/SourceFetch/SourceFetch.hpp"
#include "PlatformService/BeastServer.hpp"
#include "PlatformService/Http.hpp"
#include "PlatformService/PlatformOps/OpsManager.hpp"
#include "PlatformService/RouteCatalog.hpp"
#include "PlatformStorage/PlatformRecovery/RecoveryManager.hpp"
#include "PlatformSecurity/PlatformCA/CertificateAuthority.hpp"
#include "PlatformSecurity/ExternalIdentity/ExternalIdentity.hpp"
#include "PlatformSecurity/PlatformClientAuth/Authorization.hpp"
#include "PlatformSecurity/PlatformClientAuth/Identity.hpp"
#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"
#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"
#include "PlatformService/Router.hpp"

#include <openssl/sha.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using json = nlohmann::json;

using pafio::testsupport::MakeTempDir;
using pafio::testsupport::ReadFile;
using pafio::testsupport::WriteFile;

namespace
{

class RecordingOperatingSystemAdapter final : public pafio::platform::OperatingSystemAdapter
{
public:
  mutable std::vector<pafio::ProcessRequest> requests;
  std::map<std::string, std::string> environment;

  std::optional<std::string> GetEnv(std::string_view name) const override
  {
    const auto value = environment.find(std::string(name));
    if (value != environment.end())
    {
      return value->second;
    }
    return std::nullopt;
  }

  void CreateDirectories(const fs::path &path) const override
  {
    fs::create_directories(path);
  }

  void RemoveAll(const fs::path &path) const override
  {
    fs::remove_all(path);
  }

  void WriteTextFile(const fs::path &path, std::string_view text) const override
  {
    WriteFile(path, std::string(text));
  }

  pafio::ProcessResult RunProcess(const pafio::ProcessRequest &request) const override
  {
    requests.push_back(request);
    if (request.args.size() >= 2 && request.args[0] == "rev-parse" && request.args[1] == "HEAD")
    {
      return {.stdout_text = "abc123\n"};
    }
    return {};
  }

  std::string SendTcpRequest(const pafio::platform::TcpRequest &) const override
  {
    return {};
  }

  void SleepFor(std::chrono::milliseconds) const override {}
};

pafio::platform::MtlsIdentity WorkerIdentity()
{
  return {
      .role = "worker",
      .tenant_id = "tenant-acme",
      .node_id = "worker-01",
  };
}

pafio::platform::MtlsIdentity RegistryWriterIdentity()
{
  return {
      .role = "registry-writer",
      .tenant_id = "tenant-acme",
      .node_id = "registry-writer-01",
  };
}

pafio::platform::MtlsIdentity MirrorIdentity()
{
  return {
      .role = "mirror",
      .tenant_id = "tenant-acme",
      .node_id = "mirror-01",
  };
}

pafio::platform::MtlsIdentity OperatorIdentity()
{
  return {
      .role = "operator",
      .tenant_id = "platform",
      .node_id = "operator-01",
  };
}

nlohmann::json MinimalJobRequest()
{
  return {
      {"tenant_id", "tenant-acme"},
      {"user_id", "user-alice"},
      {"workspace_id", "workspace-main"},
      {"action", "build"},
      {"region", "local-dev"},
      {"preferred_worker_pool", "default"},
      {"job_request", {
                          {"schema_version", 1},
                          {"manifest_path", "pafio.toml"},
                          {"profile", "dev"},
                          {"source", {{"origin", "file:///tmp/styio-platform-test-workspace"}}},
                          {"workflow", json::object()},
                          {"target", json::object()},
                      }},
  };
}

pafio::platform::HttpRequest Request(
    pafio::platform::HttpMethod method,
    std::string path,
    nlohmann::json body = nlohmann::json::object())
{
  return {
      .method = method,
      .path = std::move(path),
      .body = std::move(body),
      .identity = WorkerIdentity(),
  };
}

pafio::platform::HttpRequest RequestWithIdentity(
    pafio::platform::HttpMethod method,
    std::string path,
    pafio::platform::MtlsIdentity identity,
    nlohmann::json body = nlohmann::json::object())
{
  return {
      .method = method,
      .path = std::move(path),
      .body = std::move(body),
      .identity = std::move(identity),
  };
}

pafio::platform::HttpRequest RequestWithToken(
    pafio::platform::HttpMethod method,
    std::string path,
    std::string token,
    nlohmann::json body = nlohmann::json::object())
{
  return {
      .method = method,
      .path = std::move(path),
      .headers = {{"authorization", "Bearer " + std::move(token)}},
      .body = std::move(body),
  };
}

pafio::platform::PlatformConfig TestPlatformConfig(const fs::path &root)
{
  pafio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.postgres_dsn = "postgres://platform@localhost/styio";
  config.object_store.provider = "memory";
  config.registry.root = (root / "registry").string();
  config.registry.key_dir = (root / "keys").string();
  config.registry.registry_name = "test-registry";
  config.registry.mirror_id = "mirror-local";
  config.registry.mirror_origin = "registry-primary";
  config.mtls.required = true;
  return config;
}

struct TestUstarEntry
{
  std::string path;
  std::string data;
};

std::string TestSha256Hex(std::string_view bytes)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(
      reinterpret_cast<const unsigned char *>(bytes.data()),
      bytes.size(),
      digest);
  std::ostringstream encoded;
  encoded << std::hex << std::setfill('0');
  for (const unsigned char byte : digest)
  {
    encoded << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return encoded.str();
}

std::string TestBase64Encode(std::string_view bytes)
{
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(4U * ((bytes.size() + 2U) / 3U));
  for (size_t offset = 0U; offset < bytes.size(); offset += 3U)
  {
    const uint32_t first = static_cast<unsigned char>(bytes[offset]);
    const uint32_t second =
        offset + 1U < bytes.size() ? static_cast<unsigned char>(bytes[offset + 1U]) : 0U;
    const uint32_t third =
        offset + 2U < bytes.size() ? static_cast<unsigned char>(bytes[offset + 2U]) : 0U;
    const uint32_t value = (first << 16U) | (second << 8U) | third;
    encoded.push_back(alphabet[(value >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(value >> 12U) & 0x3fU]);
    encoded.push_back(offset + 1U < bytes.size() ? alphabet[(value >> 6U) & 0x3fU] : '=');
    encoded.push_back(offset + 2U < bytes.size() ? alphabet[value & 0x3fU] : '=');
  }
  return encoded;
}

void WriteTestUstarOctal(
    std::string &header,
    const size_t offset,
    const size_t width,
    const uint64_t value)
{
  std::ostringstream encoded;
  encoded << std::oct << std::setfill('0') << std::setw(static_cast<int>(width - 1U)) << value;
  const std::string field = encoded.str();
  if (field.size() != width - 1U)
  {
    throw std::runtime_error("test ustar numeric field overflow");
  }
  header.replace(offset, width - 1U, field);
  header[offset + width - 1U] = '\0';
}

std::string BuildTestCanonicalUstar(const std::vector<TestUstarEntry> &entries)
{
  std::string archive;
  for (const TestUstarEntry &entry : entries)
  {
    if (entry.path.empty() || entry.path.size() > 100U)
    {
      throw std::runtime_error("test ustar path does not fit the name field");
    }
    std::string header(512U, '\0');
    header.replace(0U, entry.path.size(), entry.path);
    WriteTestUstarOctal(header, 100U, 8U, 0644U);
    WriteTestUstarOctal(header, 108U, 8U, 0U);
    WriteTestUstarOctal(header, 116U, 8U, 0U);
    WriteTestUstarOctal(header, 124U, 12U, entry.data.size());
    WriteTestUstarOctal(header, 136U, 12U, 0U);
    header[156] = '0';
    header.replace(257U, 6U, std::string("ustar\0", 6U));
    header.replace(263U, 2U, "00");
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    uint64_t checksum = 0U;
    for (const unsigned char byte : header)
    {
      checksum += byte;
    }
    std::ostringstream encoded_checksum;
    encoded_checksum << std::oct << std::setfill('0') << std::setw(6) << checksum;
    if (encoded_checksum.str().size() != 6U)
    {
      throw std::runtime_error("test ustar checksum overflow");
    }
    header.replace(148U, 6U, encoded_checksum.str());
    header[154] = '\0';
    header[155] = ' ';
    archive.append(header);
    archive.append(entry.data);
    archive.append((512U - entry.data.size() % 512U) % 512U, '\0');
  }
  archive.append(1024U, '\0');
  return archive;
}

std::string TestPafioManifest(
    const std::string &package,
    const std::string &version,
    const json &dependencies = json::array(),
    const json &dev_dependencies = json::array(),
    const bool publish = true)
{
  std::string manifest =
      "[pafio]\n"
      "manifest-version = 1\n\n"
      "[package]\n"
      "name = \"" + package + "\"\n"
      "version = \"" + version + "\"\n"
      "edition = \"2026\"\n"
      "publish = " + std::string(publish ? "true" : "false") + "\n\n"
      "[build]\n"
      "implicit-std = true\n\n"
      "[lib]\n"
      "path = \"src/lib.styio\"\n";
  const auto append_dependencies =
      [&manifest](const std::string &table_name, const json &entries)
      {
        if (entries.empty())
        {
          return;
        }
        std::vector<json> ordered(entries.begin(), entries.end());
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](const json &left, const json &right)
            {
              return left.at("alias").get<std::string>() < right.at("alias").get<std::string>();
            });
        manifest += "\n[" + table_name + "]\n";
        for (const json &dependency : ordered)
        {
          manifest += json(dependency.at("alias").get<std::string>()).dump() +
                      " = { package = " +
                      json(dependency.at("package").get<std::string>()).dump() +
                      ", version = " +
                      json(dependency.at("version_req").get<std::string>()).dump() +
                      ", registry = " +
                      json(dependency.at("registry").get<std::string>()).dump() +
                      " }\n";
        }
      };
  append_dependencies("dependencies", dependencies);
  append_dependencies("dev-dependencies", dev_dependencies);
  return manifest;
}

std::string BuildTestPafioArchiveWithManifest(
    const std::string &manifest,
    const std::string &package = "demo/app",
    const std::string &version = "0.1.0")
{
  const size_t slash = package.find('/');
  const std::string short_name = slash == std::string::npos ? package : package.substr(slash + 1U);
  const std::string prefix = short_name + "-" + version;
  return BuildTestCanonicalUstar({
      {prefix + "/pafio.toml", manifest},
      {prefix + "/src/lib.styio", "# app := 1\n"},
  });
}

std::string BuildTestPafioArchive(
    const std::string &package = "demo/app",
    const std::string &version = "0.1.0",
    const json &dependencies = json::array(),
    const json &dev_dependencies = json::array(),
    const bool publish = true)
{
  return BuildTestPafioArchiveWithManifest(
      TestPafioManifest(package, version, dependencies, dev_dependencies, publish),
      package,
      version);
}

json PafioPublishRequestForArchive(
    std::string archive,
    std::string package,
    std::string version,
    json dependencies,
    json dev_dependencies)
{
  return {
      {"package", std::move(package)},
      {"version", std::move(version)},
      {"archive_name", "app-0.1.0.pafio.src.tar"},
      {"archive_base64", TestBase64Encode(archive)},
      {"archive_sha256", TestSha256Hex(archive)},
      {"archive_size_bytes", archive.size()},
      {"publisher_id", "untrusted-pafio-client"},
      {"dependencies", std::move(dependencies)},
      {"dev_dependencies", std::move(dev_dependencies)},
  };
}

json PafioPublishRequest(
    std::string package = "demo/app",
    std::string version = "0.1.0",
    json dependencies = json::array(),
    json dev_dependencies = json::array())
{
  const std::string archive =
      BuildTestPafioArchive(package, version, dependencies, dev_dependencies);
  return PafioPublishRequestForArchive(
      archive,
      std::move(package),
      std::move(version),
      std::move(dependencies),
      std::move(dev_dependencies));
}

bool DirectoryHasEntries(const fs::path &path)
{
  return fs::exists(path) && fs::directory_iterator(path) != fs::directory_iterator();
}

}  // namespace

TEST(PlatformSourceFetchTests, AllowsStandardGitTransports)
{
  const pafio::GitSourcePolicy policy = pafio::PublicGitSourcePolicy();

  EXPECT_FALSE(pafio::GitSourcePolicyViolation("https://github.com/acme/demo.git", policy).has_value());
  EXPECT_FALSE(pafio::GitSourcePolicyViolation("http://git.local/acme/demo.git", policy).has_value());
  EXPECT_FALSE(pafio::GitSourcePolicyViolation("git@github.com:acme/demo.git", policy).has_value());
  EXPECT_FALSE(pafio::GitSourcePolicyViolation("ssh://github.com/acme/demo.git", policy).has_value());
  EXPECT_FALSE(pafio::GitSourcePolicyViolation("git://github.com/acme/demo.git", policy).has_value());
  EXPECT_FALSE(pafio::GitSourcePolicyViolation("file:///tmp/demo.git", policy).has_value());
  EXPECT_FALSE(pafio::GitSourcePolicyViolation("../demo.git", policy).has_value());
  EXPECT_TRUE(pafio::GitSourcePolicyViolation("ftp://example.test/demo.git", policy).has_value());
  EXPECT_TRUE(pafio::GitSourcePolicyViolation("https://github.com/acme/demo.git\n", policy).has_value());
}

TEST(PlatformSourceFetchTests, ClonesPublicWorktreeThroughSharedFetcher)
{
  const fs::path root = MakeTempDir("platform-source-fetch-worktree");
  RecordingOperatingSystemAdapter os;
  const pafio::GitSourceFetcher fetcher(os);

  const pafio::GitWorktreeResult result = fetcher.EnsureWorktree({
      .origin = "https://github.com/acme/demo.git",
      .checkout_root = root / "checkout",
      .revision = std::string("main"),
      .update_existing = true,
      .shallow = true,
      .depth = 1,
      .clone_revision_as_branch = false,
      .policy = pafio::PublicGitSourcePolicy(),
      .error_context = "test source fetch",
  });

  ASSERT_EQ(os.requests.size(), 4U);
  EXPECT_EQ(os.requests[0].program, "git");
  EXPECT_EQ(os.requests[0].args, std::vector<std::string>({
                                    "clone",
                                    "--depth",
                                    "1",
                                    "--no-checkout",
                                    "https://github.com/acme/demo.git",
                                    (root / "checkout").string(),
                                }));
  EXPECT_EQ(os.requests[1].args, std::vector<std::string>({"fetch", "--depth", "1", "origin", "main"}));
  EXPECT_EQ(os.requests[1].working_directory.value(), root / "checkout");
  EXPECT_EQ(os.requests[2].args, std::vector<std::string>({"checkout", "--force", "FETCH_HEAD"}));
  EXPECT_EQ(os.requests[3].args, std::vector<std::string>({"rev-parse", "HEAD"}));
  EXPECT_TRUE(result.cloned);
  EXPECT_TRUE(result.fetched);
  EXPECT_TRUE(result.checked_out);
  EXPECT_EQ(result.resolved_revision, "abc123");
}

TEST(PlatformClientAuthTests, ParsesMtlsUriSanIntoRoleTenantAndNode)
{
  const std::optional<pafio::platform::MtlsIdentity> identity =
      pafio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/tenant-acme/role/worker/node/worker-01");

  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->role, "worker");
  EXPECT_EQ(identity->tenant_id, "tenant-acme");
  EXPECT_EQ(identity->node_id, "worker-01");
  EXPECT_TRUE(pafio::platform::IsPlatformServiceRole(identity->role));

  const json serialized = pafio::platform::SerializeMtlsIdentity(*identity);
  EXPECT_EQ(serialized.at("role").get<std::string>(), "worker");
  EXPECT_EQ(serialized.at("tenant_id").get<std::string>(), "tenant-acme");
  EXPECT_EQ(serialized.at("node_id").get<std::string>(), "worker-01");
}

TEST(PlatformClientAuthTests, RejectsUnknownMtlsUriSanRoleOrMissingNode)
{
  EXPECT_FALSE(pafio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/acme/role/browser/node/client").has_value());
  EXPECT_FALSE(pafio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/acme/role/worker").has_value());
  EXPECT_FALSE(pafio::platform::ParseMtlsUriSan("https://styio-platform/tenant/acme/role/worker/node/worker-01").has_value());
}

TEST(PlatformClientAuthTests, AppliesOperationAuthorizationPolicy)
{
  const pafio::platform::MtlsIdentity registry_writer{
      .role = "registry-writer",
      .tenant_id = "tenant-acme",
      .node_id = "registry-writer-01",
  };
  const pafio::platform::MtlsIdentity worker{
      .role = "worker",
      .tenant_id = "tenant-acme",
      .node_id = "worker-01",
  };

  EXPECT_TRUE(pafio::platform::IsInternalRole(worker));
  EXPECT_TRUE(pafio::platform::IsAuthorizedForOperation("publishRelease", registry_writer));
  EXPECT_FALSE(pafio::platform::IsAuthorizedForOperation("publishRelease", worker));
  EXPECT_FALSE(pafio::platform::IsAuthorizedForOperation("registerWorkgroupCluster", registry_writer));
  EXPECT_TRUE(pafio::platform::IsAuthorizedForOperation("claimJob", worker));
}

TEST(PlatformExternalIdentityTests, NormalizesSupportedProviders)
{
  const pafio::platform::ExternalIdentityRecord google = pafio::platform::NormalizeExternalIdentity({
      {"provider", "google"},
      {"tenant_id", "tenant-acme"},
      {"roles", json::array({"developer"})},
      {"claims", {{"sub", "google-subject-01"}, {"email", "alice@example.test"}}},
  });
  EXPECT_EQ(google.actor_id, "external:google:google-subject-01");
  EXPECT_EQ(google.email, "alice@example.test");

  const pafio::platform::ExternalIdentityRecord telegram = pafio::platform::NormalizeExternalIdentity({
      {"provider", "telegram"},
      {"claims", {{"id", 100200300}, {"username", "alice_dev"}}},
  });
  EXPECT_EQ(telegram.actor_id, "external:telegram:100200300");
  EXPECT_EQ(telegram.email, "alice_dev");

  EXPECT_THROW(
      pafio::platform::NormalizeExternalIdentity({{"provider", "unknown"}, {"claims", {{"sub", "x"}}}}),
      std::runtime_error);
}

TEST(PlatformCATests, InitializesLocalCaAndIssuesMtlsCertificate)
{
  const fs::path root = MakeTempDir("platform-local-ca");
  pafio::platform::PlatformCertificateAuthorityConfig config;
  config.root_dir = root / "mtls";
  config.ca_valid_days = 30;
  config.leaf_valid_days = 7;

  const pafio::platform::PlatformCertificateSubject subject =
      pafio::platform::BuildPlatformNodeCertificateSubject("worker", "tenant-acme", "worker-01");
  const pafio::platform::PlatformCertificateBundle bundle =
      pafio::platform::EnsurePlatformMtlsCertificate(config, subject);

  EXPECT_TRUE(fs::is_regular_file(bundle.ca_certificate_path));
  EXPECT_TRUE(fs::is_regular_file(bundle.ca_private_key_path));
  EXPECT_TRUE(fs::is_regular_file(bundle.certificate_path));
  EXPECT_TRUE(fs::is_regular_file(bundle.private_key_path));
  EXPECT_EQ(
      bundle.identity_uri_san,
      "spiffe://styio-platform/tenant/tenant-acme/role/worker/node/worker-01");

  const std::optional<pafio::platform::MtlsIdentity> identity =
      pafio::platform::ParseMtlsUriSan(bundle.identity_uri_san);
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->role, "worker");
  EXPECT_EQ(identity->tenant_id, "tenant-acme");
  EXPECT_EQ(identity->node_id, "worker-01");
  EXPECT_NE(ReadFile(bundle.ca_certificate_path).find("BEGIN CERTIFICATE"), std::string::npos);
  EXPECT_NE(ReadFile(bundle.private_key_path).find("BEGIN PRIVATE KEY"), std::string::npos);
}

TEST(PlatformServiceRouterTests, MatchesRouteParametersForJobsAndMirrors)
{
  const std::vector<pafio::platform::RouteSpec> routes = pafio::platform::BuildPlatformControlPlaneRoutes();

  const std::optional<pafio::platform::RouteMatch> job =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Get, "/jobs/job-abc/events");
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->route.operation_id, "getJobEvents");
  EXPECT_EQ(job->parameters.at("job_id"), "job-abc");

  const std::optional<pafio::platform::RouteMatch> mirror =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Get, "/mirrors/registry-primary/status");
  ASSERT_TRUE(mirror.has_value());
  EXPECT_EQ(mirror->route.operation_id, "mirrorStatus");
  EXPECT_EQ(mirror->parameters.at("mirror_id"), "registry-primary");

  const std::optional<pafio::platform::RouteMatch> docs_governance =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Get, "/docs/governance");
  ASSERT_TRUE(docs_governance.has_value());
  EXPECT_EQ(docs_governance->route.operation_id, "listDocumentationGovernance");
  EXPECT_TRUE(docs_governance->route.internal);

  const std::optional<pafio::platform::RouteMatch> docs_plan =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/docs/change-plan");
  ASSERT_TRUE(docs_plan.has_value());
  EXPECT_EQ(docs_plan->route.operation_id, "planDocumentationChange");
  EXPECT_TRUE(docs_plan->route.internal);

  const std::optional<pafio::platform::RouteMatch> ecosystem =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Get, "/ecosystem/repositories");
  ASSERT_TRUE(ecosystem.has_value());
  EXPECT_EQ(ecosystem->route.operation_id, "listEcosystemRepositories");
  EXPECT_TRUE(ecosystem->route.internal);

  const std::optional<pafio::platform::RouteMatch> release_plan =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/ecosystem/releases/plan");
  ASSERT_TRUE(release_plan.has_value());
  EXPECT_EQ(release_plan->route.operation_id, "planEcosystemRelease");
  EXPECT_TRUE(release_plan->route.internal);

  const std::optional<pafio::platform::RouteMatch> register_cluster =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/workgroups/local-dev/clusters/register");
  ASSERT_TRUE(register_cluster.has_value());
  EXPECT_EQ(register_cluster->route.operation_id, "registerWorkgroupCluster");
  EXPECT_EQ(register_cluster->parameters.at("workgroup_id"), "local-dev");

  const std::optional<pafio::platform::RouteMatch> list_clusters =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Get, "/workgroups/local-dev/clusters");
  ASSERT_TRUE(list_clusters.has_value());
  EXPECT_EQ(list_clusters->route.operation_id, "listWorkgroupClusters");
  EXPECT_TRUE(list_clusters->route.internal);

  const std::optional<pafio::platform::RouteMatch> register_container =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/compile-containers/register");
  ASSERT_TRUE(register_container.has_value());
  EXPECT_EQ(register_container->route.operation_id, "registerCompileContainer");
  EXPECT_TRUE(register_container->route.internal);

  const std::optional<pafio::platform::RouteMatch> switch_container =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/compile-containers/container-01/switch-workspace");
  ASSERT_TRUE(switch_container.has_value());
  EXPECT_EQ(switch_container->route.operation_id, "switchCompileContainerWorkspace");
  EXPECT_EQ(switch_container->parameters.at("container_id"), "container-01");

  const std::optional<pafio::platform::RouteMatch> snapshot =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/ops/recovery/snapshots");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->route.operation_id, "createRecoverySnapshot");

  const std::optional<pafio::platform::RouteMatch> restore =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/ops/recovery/snapshots/snap-000001/restore");
  ASSERT_TRUE(restore.has_value());
  EXPECT_EQ(restore->route.operation_id, "restoreRecoverySnapshot");
  EXPECT_EQ(restore->parameters.at("snapshot_id"), "snap-000001");

  const std::optional<pafio::platform::RouteMatch> external_identity =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/identity/external/exchange");
  ASSERT_TRUE(external_identity.has_value());
  EXPECT_EQ(external_identity->route.operation_id, "exchangeExternalIdentity");
  EXPECT_FALSE(external_identity->route.internal);

  EXPECT_FALSE(pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/jobs/job-abc/events").has_value());
}

TEST(PlatformServiceRouterTests, MatchesRegistryControlPlaneRoutesWithContractBasePath)
{
  const std::vector<pafio::platform::RouteSpec> routes = pafio::platform::BuildRegistryControlPlaneRoutes();

  const std::optional<pafio::platform::RouteMatch> status =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Get, "/api/pafio-registry-control/v1/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->route.operation_id, "registryStatus");
  EXPECT_TRUE(status->route.internal);

  const std::optional<pafio::platform::RouteMatch> descriptor =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Get, "/api/pafio-registry-control/v1/descriptor");
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->route.operation_id, "registryDescriptor");
  EXPECT_TRUE(descriptor->route.internal);

  const std::optional<pafio::platform::RouteMatch> publish =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/api/pafio-registry-control/v1/publish");
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(publish->route.operation_id, "publishRelease");

  const std::optional<pafio::platform::RouteMatch> verify =
      pafio::platform::MatchRoute(routes, pafio::platform::HttpMethod::Post, "/api/pafio-registry-control/v1/verify");
  ASSERT_TRUE(verify.has_value());
  EXPECT_EQ(verify->route.operation_id, "verifyRegistry");

  const std::optional<pafio::platform::RouteMatch> release =
      pafio::platform::MatchRoute(
          routes,
          pafio::platform::HttpMethod::Get,
          "/api/pafio-registry-control/v1/packages/demo/app/releases/0.1.0");
  ASSERT_TRUE(release.has_value());
  EXPECT_EQ(release->route.operation_id, "getPackageRelease");
  EXPECT_EQ(release->parameters.at("namespace"), "demo");
  EXPECT_EQ(release->parameters.at("name"), "app");
  EXPECT_EQ(release->parameters.at("version"), "0.1.0");

  const std::optional<pafio::platform::RouteMatch> remove_owner =
      pafio::platform::MatchRoute(
          routes,
          pafio::platform::HttpMethod::Delete,
          "/api/pafio-registry-control/v1/packages/demo/app/owners/user-bob");
  ASSERT_TRUE(remove_owner.has_value());
  EXPECT_EQ(remove_owner->route.operation_id, "removePackageOwner");

  const std::optional<pafio::platform::RouteMatch> rollout =
      pafio::platform::MatchRoute(
          routes,
          pafio::platform::HttpMethod::Post,
          "/api/pafio-registry-control/v1/release-channels/canary/rollout");
  ASSERT_TRUE(rollout.has_value());
  EXPECT_EQ(rollout->route.operation_id, "rolloutReleaseChannel");
  EXPECT_EQ(rollout->parameters.at("channel"), "canary");
}

TEST(PlatformHttpAdapterTests, AppliesLargeBodyLimitOnlyToPafioPublishTarget)
{
  using pafio::platform::kDefaultHttpRequestBodyLimitBytes;
  using pafio::platform::kPublishHttpRequestBodyLimitBytes;

  EXPECT_EQ(
      pafio::platform::HttpRequestBodyLimitForTarget("/api/pafio-registry-control/v1/publish"),
      kPublishHttpRequestBodyLimitBytes);
  EXPECT_EQ(
      pafio::platform::HttpRequestBodyLimitForTarget("/api/pafio-registry-control/v1/publish?request_id=1"),
      kPublishHttpRequestBodyLimitBytes);
  EXPECT_EQ(
      pafio::platform::HttpRequestBodyLimitForTarget("/api/pafio-registry-control/v1/verify"),
      kDefaultHttpRequestBodyLimitBytes);
  EXPECT_EQ(
      pafio::platform::HttpRequestBodyLimitForTarget("/api/pafio-registry-control/v1/publish/extra"),
      kDefaultHttpRequestBodyLimitBytes);

  EXPECT_TRUE(pafio::platform::HttpRequestBodySizeAllowed(
      "/api/pafio-registry-control/v1/publish",
      kPublishHttpRequestBodyLimitBytes));
  EXPECT_FALSE(pafio::platform::HttpRequestBodySizeAllowed(
      "/api/pafio-registry-control/v1/publish",
      static_cast<uint64_t>(kPublishHttpRequestBodyLimitBytes) + 1U));
  EXPECT_TRUE(pafio::platform::HttpRequestBodySizeAllowed(
      "/api/pafio-registry-control/v1/verify",
      kDefaultHttpRequestBodyLimitBytes));
  EXPECT_FALSE(pafio::platform::HttpRequestBodySizeAllowed(
      "/api/pafio-registry-control/v1/verify",
      static_cast<uint64_t>(kDefaultHttpRequestBodyLimitBytes) + 1U));
}

TEST(PlatformHttpAdapterTests, RejectsOverflowWhenPlanningBoundedRequestSize)
{
  const std::optional<size_t> ordinary = pafio::platform::CheckedHttpRequestSize(128U, 256U);
  ASSERT_TRUE(ordinary.has_value());
  EXPECT_EQ(*ordinary, 384U);
  EXPECT_FALSE(pafio::platform::CheckedHttpRequestSize(
      std::numeric_limits<size_t>::max(),
      1U).has_value());
  if constexpr (sizeof(size_t) < sizeof(uint64_t))
  {
    EXPECT_FALSE(pafio::platform::CheckedHttpRequestSize(
        0U,
        static_cast<uint64_t>(std::numeric_limits<size_t>::max()) + 1U).has_value());
  }
}

TEST(PlatformPersistenceObjectStoreTests, SanitizesArtifactObjectKeyParts)
{
  const std::string key = pafio::platform::BuildArtifactObjectKey(
      "tenant/acme",
      "workspace main",
      "job:42",
      "../out.tar.gz");

  EXPECT_EQ(key, "tenants/tenant_acme/workspaces/workspace_main/jobs/job_42/artifacts/.._out.tar.gz");
  EXPECT_EQ(key.find("tenant/acme"), std::string::npos);
  EXPECT_EQ(key.find("workspace main"), std::string::npos);
  EXPECT_EQ(key.find("job:42"), std::string::npos);
  EXPECT_EQ(key.find("../out"), std::string::npos);
}

TEST(PlatformPersistenceObjectStoreTests, NormalizesObjectKeysAsCanonicalRelativePaths)
{
  EXPECT_EQ(pafio::platform::NormalizeObjectKey("/index/demo/app.jsonl"), "index/demo/app.jsonl");
  EXPECT_THROW(pafio::platform::NormalizeObjectKey(""), std::runtime_error);
  EXPECT_THROW(pafio::platform::NormalizeObjectKey("index/../root.json"), std::runtime_error);
  EXPECT_THROW(pafio::platform::NormalizeObjectKey("index//root.json"), std::runtime_error);
  EXPECT_THROW(pafio::platform::NormalizeObjectKey("index\\root.json"), std::runtime_error);
}

TEST(PlatformRecoveryTests, CreatesAndRestoresFilesystemSnapshots)
{
  const fs::path root = MakeTempDir("platform-recovery");
  WriteFile(root / "registry/config.json", "{\"registry\":\"test\"}\n");
  WriteFile(root / "registry/index/demo/app.jsonl", "{\"version\":\"0.1.0\"}\n");

  const json snapshot = pafio::platform::CreateFilesystemSnapshot({
      .source_root = root / "registry",
      .snapshots_root = root / "snapshots",
      .snapshot_id = "snap-000001",
      .label = "before-change",
      .created_at = "2026-05-09T00:00:00Z",
  });
  EXPECT_EQ(snapshot.at("snapshot_id").get<std::string>(), "snap-000001");
  EXPECT_EQ(snapshot.at("file_count").get<int>(), 2);

  WriteFile(root / "registry/config.json", "{\"registry\":\"changed\"}\n");
  const json restored = pafio::platform::RestoreFilesystemSnapshot(
      root / "snapshots",
      "snap-000001",
      root / "registry",
      "2026-05-09T00:01:00Z");
  EXPECT_TRUE(restored.at("verified").get<bool>());
  EXPECT_NE(ReadFile(root / "registry/config.json").find("\"test\""), std::string::npos);
}

TEST(PlatformOpsTests, RateLimiterAndMetricsTrackRequests)
{
  pafio::platform::PlatformRateLimiter limiter;
  EXPECT_TRUE(limiter.Allow("actor", "publishRelease", 2, 60, 10));
  EXPECT_TRUE(limiter.Allow("actor", "publishRelease", 2, 60, 11));
  EXPECT_FALSE(limiter.Allow("actor", "publishRelease", 2, 60, 12));
  EXPECT_TRUE(limiter.Allow("actor", "publishRelease", 2, 60, 75));

  pafio::platform::PlatformRequestMetrics metrics;
  metrics.Record("publishRelease", 200);
  metrics.Record("publishRelease", 409);
  const json snapshot = metrics.Snapshot();
  EXPECT_EQ(snapshot.at("total_requests").get<int>(), 2);
  EXPECT_EQ(snapshot.at("by_operation").at("publishRelease").get<int>(), 2);
  EXPECT_EQ(snapshot.at("by_status").at("409").get<int>(), 1);
}

TEST(PlatformPersistencePostgresTests, DefinesCloudKernelMigrationAndClaimSql)
{
  const std::vector<pafio::platform::SqlMigration> migrations = pafio::platform::CloudKernelMigrations();

  ASSERT_FALSE(migrations.empty());
  EXPECT_EQ(migrations.front().id, "001_cloud_kernel");
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_jobs"), std::string::npos);
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_job_events"), std::string::npos);
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_artifacts"), std::string::npos);
  EXPECT_NE(migrations.front().sql.find("user_id TEXT"), std::string::npos);
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_compile_containers"), std::string::npos);
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_workgroup_clusters"), std::string::npos);
  ASSERT_GE(migrations.size(), 2U);
  EXPECT_EQ(migrations.at(1).id, "002_user_bound_compile_containers");
  ASSERT_GE(migrations.size(), 3U);
  EXPECT_EQ(migrations.at(2).id, "003_package_registry_repository_model");
  EXPECT_NE(migrations.at(2).sql.find("CREATE TABLE IF NOT EXISTS platform_packages"), std::string::npos);
  EXPECT_NE(migrations.at(2).sql.find("CREATE TABLE IF NOT EXISTS platform_package_releases"), std::string::npos);
  EXPECT_NE(migrations.at(2).sql.find("CREATE TABLE IF NOT EXISTS platform_publications"), std::string::npos);
  EXPECT_NE(migrations.at(2).sql.find("CREATE TABLE IF NOT EXISTS platform_distributions"), std::string::npos);
  EXPECT_NE(migrations.at(2).sql.find("CREATE TABLE IF NOT EXISTS platform_registry_audit_events"), std::string::npos);
  EXPECT_NE(migrations.at(2).sql.find("CREATE SEQUENCE IF NOT EXISTS platform_publish_token_id_seq"), std::string::npos);
  EXPECT_NE(migrations.at(2).sql.find("CREATE SEQUENCE IF NOT EXISTS platform_registry_audit_event_id_seq"), std::string::npos);

  const std::string claim_sql = pafio::platform::ClaimJobSql();
  EXPECT_NE(claim_sql.find("FOR UPDATE SKIP LOCKED"), std::string::npos);
  EXPECT_NE(claim_sql.find("worker_pool_key = $2"), std::string::npos);
  EXPECT_NE(claim_sql.find("RETURNING *"), std::string::npos);

  const std::string complete_sql = pafio::platform::CompleteJobSql();
  EXPECT_NE(complete_sql.find("status = 'running'"), std::string::npos);
  EXPECT_NE(complete_sql.find("worker_id = $4"), std::string::npos);
}

TEST(PlatformPersistenceRegistryTests, SerializesPackageRegistryDomainRecords)
{
  const pafio::platform::RegistryPackageRecord package{
      .package_id = "demo/app",
      .package_namespace = "demo",
      .name = "app",
      .created_at = "2026-05-09T00:00:00Z",
      .created_by = "user-alice",
      .visibility = "public",
  };
  EXPECT_EQ(pafio::platform::SerializeRegistryPackageRecord(package).at("namespace").get<std::string>(), "demo");

  const pafio::platform::RegistryPackageReleaseRecord release{
      .package_id = "demo/app",
      .version = "0.1.0",
      .edition = "2026",
      .manifest_sha256 = std::string(64, '1'),
      .source_artifact_sha256 = std::string(64, '2'),
      .dependencies = json::array(),
      .publisher_id = "user-alice",
      .published_at = "2026-05-09T00:00:00Z",
      .yanked = true,
      .yanked_reason = "metadata correction",
  };
  EXPECT_TRUE(pafio::platform::SerializeRegistryPackageReleaseRecord(release).at("yanked").get<bool>());

  const pafio::platform::RegistryPublishTokenRecord token{
      .token_id = "tok-000000000001",
      .token_hash = std::string(64, 'a'),
      .owner_id = "user-alice",
      .scopes = {"package:publish"},
      .package_patterns = {"demo/*"},
      .expires_at = "2026-06-09T00:00:00Z",
      .revoked_at = "",
      .created_at = "2026-05-09T00:00:00Z",
  };
  EXPECT_FALSE(pafio::platform::SerializeRegistryPublishTokenRecord(token).contains("token_hash"));
  EXPECT_TRUE(pafio::platform::SerializeRegistryPublishTokenRecord(token, true).contains("token_hash"));

  const pafio::platform::RegistryPublicationRecord publication{
      .publication_id = "pub-000001",
      .repository_version_id = "rv-000001",
      .layout_version = 2,
      .root_path = "/registry/_publications/pub-000001",
      .manifest_sha256 = std::string(64, 'b'),
      .tree_size = 1,
      .created_at = "2026-05-09T00:00:00Z",
      .verified = true,
  };
  EXPECT_EQ(pafio::platform::SerializeRegistryPublicationRecord(publication).at("layout_version").get<int>(), 2);

  const pafio::platform::RegistryAuditEventRecord audit{
      .event_id = "audit-000000000001",
      .actor_id = "user-alice",
      .operation = "publishRelease",
      .target = {{"package_id", "demo/app"}},
      .request_id = "request-01",
      .result = "success",
      .created_at = "2026-05-09T00:00:00Z",
  };
  EXPECT_EQ(pafio::platform::SerializeRegistryAuditEventRecord(audit).at("operation").get<std::string>(), "publishRelease");
}

TEST(PlatformPersistenceFactoryTests, BuildsCompileContainerRecordFromRegistrationPayload)
{
  const pafio::platform::CompileContainerRecordFactory factory;
  const pafio::platform::CompileContainerRecord record = factory.CreateFromRegistration({
      {"container_id", "container-01"},
      {"worker_id", "worker-01"},
      {"tenant_id", "tenant-acme"},
      {"user_id", "user-alice"},
      {"workspace_id", "workspace-main"},
      {"region", "local-dev"},
      {"worker_pool_key", "default"},
      {"capacity", 2},
  });

  EXPECT_EQ(record.container_id, "container-01");
  EXPECT_EQ(record.user_id, "user-alice");
  EXPECT_EQ(record.current_workspace_id, "workspace-main");
  EXPECT_EQ(record.capacity, 2);
  EXPECT_EQ(record.status, "active");
}

TEST(PlatformCloudWorkerFactoryTests, CreatesCompileContainerRegistrationPayload)
{
  pafio::platform::PlatformConfig platform;
  platform.region = "local-dev";

  pafio::platform::WorkerRuntimeConfig worker;
  worker.worker_id = "worker-01";
  worker.worker_pool_key = "default";
  worker.compile_container_id = "container-01";
  worker.compile_container_tenant_id = "tenant-acme";
  worker.compile_container_user_id = "user-alice";
  worker.compile_container_workspace_id = "workspace-main";
  worker.compile_container_capacity = 2;

  const pafio::platform::WorkerCompileContainerFactory factory;
  const pafio::platform::WorkerCompileContainerSpec spec = factory.Create(worker, platform);

  ASSERT_TRUE(spec.enabled);
  const json payload = spec.RegistrationRequest();
  EXPECT_EQ(payload.at("container_id").get<std::string>(), "container-01");
  EXPECT_EQ(payload.at("user_id").get<std::string>(), "user-alice");
  EXPECT_EQ(payload.at("workspace_id").get<std::string>(), "workspace-main");
  EXPECT_EQ(payload.at("region").get<std::string>(), "local-dev");
  EXPECT_EQ(payload.at("capacity").get<int>(), 2);

  worker.compile_container_user_id.clear();
  EXPECT_THROW(factory.Create(worker, platform), std::runtime_error);
}

TEST(PlatformCloudWorkerFactoryTests, LoadsPafioAndSystemStyioRuntimeEntrypoints)
{
  RecordingOperatingSystemAdapter os;
  os.environment = {
      {"STYIO_PLATFORM_WORKER_PAFIO_BIN", "/opt/pafio/bin/pafio"},
      {"STYIO_PLATFORM_WORKER_STYIO_BIN", "/usr/bin/styio"},
  };

  const pafio::platform::WorkerRuntimeConfig worker =
      pafio::platform::LoadWorkerRuntimeConfig(os);

  EXPECT_EQ(worker.pafio_bin, "/opt/pafio/bin/pafio");
  EXPECT_EQ(worker.styio_bin, "/usr/bin/styio");
}

TEST(PlatformCloudWorkerFactoryTests, BuildsPafioCommandWithSystemStyioEnvironment)
{
  pafio::platform::PlatformConfig platform;
  platform.region = "eu-test-1";

  pafio::platform::WorkerRuntimeConfig worker;
  worker.pafio_bin = "/usr/local/bin/pafio";
  worker.styio_bin = "/usr/local/bin/styio";
  worker.compile_container_id = "container-01";

  const pafio::platform::WorkerWorkspace workspace{
      .manifest_path = "pafio.toml",
      .checkout_root = "/workspace/source",
  };
  const json job_request = {
      {"manifest_path", "pafio.toml"},
      {"profile", "release"},
      {"workflow", {{"dry_run", true}, {"frozen", true}}},
  };

  const pafio::ProcessRequest request =
      pafio::platform::BuildWorkerPafioProcessRequest(
          worker,
          platform,
          workspace,
          job_request);

  EXPECT_EQ(request.program, "/usr/local/bin/pafio");
  EXPECT_EQ(
      request.args,
      std::vector<std::string>({
          "build",
          "--manifest-path",
          "pafio.toml",
          "--dry-run",
          "--frozen",
          "--profile",
          "release",
      }));
  ASSERT_TRUE(request.working_directory.has_value());
  EXPECT_EQ(*request.working_directory, fs::path("/workspace/source"));
  ASSERT_TRUE(request.environment_overrides.at("PAFIO_STYIO_BIN").has_value());
  EXPECT_EQ(
      *request.environment_overrides.at("PAFIO_STYIO_BIN"),
      "/usr/local/bin/styio");
  EXPECT_EQ(
      *request.environment_overrides.at("STYIO_PLATFORM_REGION"),
      "eu-test-1");
  EXPECT_EQ(
      *request.environment_overrides.at("STYIO_PLATFORM_COMPILE_CONTAINER_ID"),
      "container-01");
  EXPECT_EQ(request.error_context, "pafio build for platform worker");
}

TEST(PlatformCloudWorkerFactoryTests, CreatesWorkspaceUnderContainerRoot)
{
  const fs::path root = MakeTempDir("platform-worker-workspace-factory");

  pafio::platform::WorkerRuntimeConfig worker;
  worker.workspace_root = root / "workspaces";
  worker.artifact_root = root / "artifacts";
  worker.compile_container_id = "container-01";

  const json job = {
      {"job_id", "job-0001"},
      {"tenant_id", "tenant-acme"},
      {"workspace_id", "workspace-main"},
      {"job_request", {{"manifest_path", "pafio.toml"}}},
  };

  const pafio::platform::WorkerWorkspaceFactory factory(pafio::platform::DefaultOperatingSystemAdapter());
  const pafio::platform::WorkerWorkspace workspace = factory.Create(worker, job);

  EXPECT_EQ(workspace.manifest_path, fs::path("pafio.toml"));
  EXPECT_EQ(workspace.checkout_root, worker.workspace_root / "containers" / "container-01" / "source");
  EXPECT_EQ(workspace.stdout_path.filename(), "stdout.log");
  EXPECT_TRUE(fs::exists(workspace.checkout_root.parent_path()));
  EXPECT_TRUE(fs::exists(workspace.artifact_root));

  json absolute_manifest = job;
  absolute_manifest["job_request"]["manifest_path"] = "/tmp/pafio.toml";
  EXPECT_THROW(factory.Create(worker, absolute_manifest), std::runtime_error);
}

TEST(PlatformServiceJobQueueTests, SubmitClaimCompleteLifecycleUsesSuccessEnvelopes)
{
  pafio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.postgres_dsn = "postgres://platform@localhost/styio";
  config.object_store.provider = "memory";
  config.mtls.required = true;

  pafio::platform::PlatformRouter router(config);

  const pafio::platform::HttpResponse submit =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));
  ASSERT_EQ(submit.status_code, 200);
  ASSERT_EQ(submit.body.at("returncode").get<int>(), 0);
  EXPECT_EQ(submit.body.at("message").get<std::string>(), "queued platform job");
  const json queued = submit.body.at("payload");
  const std::string job_id = queued.at("job_id").get<std::string>();
  EXPECT_EQ(queued.at("status").get<std::string>(), "queued");
  EXPECT_EQ(queued.at("user_id").get<std::string>(), "user-alice");
  EXPECT_EQ(queued.at("worker_pool_key").get<std::string>(), "default");

  const pafio::platform::HttpResponse register_worker =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/workers/register",
          {
              {"worker_id", "worker-01"},
              {"region", "local-dev"},
              {"worker_pool_key", "default"},
              {"capacity", 1},
          }));
  ASSERT_EQ(register_worker.status_code, 200);
  EXPECT_EQ(register_worker.body.at("payload").at("status").get<std::string>(), "registered");

  const pafio::platform::HttpResponse claim =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/jobs/claim",
          {
              {"worker_id", "worker-01"},
              {"region", "local-dev"},
              {"worker_pool_key", "default"},
          }));
  ASSERT_EQ(claim.status_code, 200);
  ASSERT_TRUE(claim.body.at("payload").at("claimed").get<bool>());
  EXPECT_EQ(claim.body.at("payload").at("job").at("job_id").get<std::string>(), job_id);
  EXPECT_EQ(claim.body.at("payload").at("job").at("status").get<std::string>(), "running");
  EXPECT_EQ(claim.body.at("payload").at("job").at("worker_id").get<std::string>(), "worker-01");
  EXPECT_EQ(claim.body.at("payload").at("job").at("job_request").at("manifest_path").get<std::string>(), "pafio.toml");

  const std::string artifact_key = pafio::platform::BuildArtifactObjectKey("tenant-acme", "workspace-main", job_id, "stdout.log");
  const pafio::platform::HttpResponse complete =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/jobs/" + job_id + "/complete",
          {
              {"worker_id", "worker-01"},
              {"status", "succeeded"},
              {"message", "build completed"},
              {"artifacts", json::array({
                                {
                                    {"artifact_id", "stdout"},
                                    {"object_key", artifact_key},
                                    {"kind", "log"},
                                },
                            })},
          }));
  ASSERT_EQ(complete.status_code, 200);
  ASSERT_EQ(complete.body.at("returncode").get<int>(), 0);
  EXPECT_EQ(complete.body.at("message").get<std::string>(), "completed platform job");
  EXPECT_EQ(complete.body.at("payload").at("status").get<std::string>(), "succeeded");
  EXPECT_EQ(complete.body.at("payload").at("artifacts").at(0).at("object_key").get<std::string>(), artifact_key);

  const pafio::platform::HttpResponse events =
      router.Dispatch(Request(pafio::platform::HttpMethod::Get, "/jobs/" + job_id + "/events"));
  ASSERT_EQ(events.status_code, 200);
  const json event_list = events.body.at("payload").at("events");
  ASSERT_EQ(event_list.size(), 3U);
  EXPECT_EQ(event_list.at(0).at("status").get<std::string>(), "queued");
  EXPECT_EQ(event_list.at(1).at("status").get<std::string>(), "running");
  EXPECT_EQ(event_list.at(2).at("status").get<std::string>(), "succeeded");
}

TEST(PlatformServiceJobQueueTests, UsesPlatformPoolDefaultAndRejectsUnknownJobFields)
{
  pafio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.object_store.provider = "memory";
  config.mtls.required = true;

  pafio::platform::PlatformRouter router(config);
  json default_pool_request = MinimalJobRequest();
  default_pool_request.erase("preferred_worker_pool");
  const pafio::platform::HttpResponse accepted =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/jobs",
          default_pool_request));
  ASSERT_EQ(accepted.status_code, 200);
  EXPECT_EQ(
      accepted.body.at("payload").at("worker_pool_key").get<std::string>(),
      "default");

  json unknown_field_request = MinimalJobRequest();
  unknown_field_request["job_request"]["unsupported"] = json::object();
  const pafio::platform::HttpResponse rejected =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/jobs",
          unknown_field_request));
  ASSERT_EQ(rejected.status_code, 400);
  EXPECT_EQ(
      rejected.body.at("error_payload").at("detail").get<std::string>(),
      "job_request.unsupported is not part of the v1 contract");
}

TEST(PlatformServiceJobQueueTests, CompileContainerHotSwitchesWorkspaceWithinUserBinding)
{
  pafio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.object_store.provider = "memory";
  config.mtls.required = true;

  pafio::platform::PlatformRouter router(config);

  const pafio::platform::HttpResponse register_worker =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/workers/register",
          {
              {"worker_id", "worker-01"},
              {"region", "local-dev"},
              {"worker_pool_key", "default"},
              {"capacity", 1},
          }));
  ASSERT_EQ(register_worker.status_code, 200);

  const pafio::platform::HttpResponse register_container =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/compile-containers/register",
          {
              {"container_id", "container-01"},
              {"worker_id", "worker-01"},
              {"tenant_id", "tenant-acme"},
              {"user_id", "user-alice"},
              {"workspace_id", "workspace-main"},
              {"region", "local-dev"},
              {"worker_pool_key", "default"},
              {"capacity", 1},
          }));
  ASSERT_EQ(register_container.status_code, 200);
  EXPECT_EQ(register_container.body.at("payload").at("user_id").get<std::string>(), "user-alice");
  EXPECT_EQ(register_container.body.at("payload").at("current_workspace_id").get<std::string>(), "workspace-main");

  const pafio::platform::HttpResponse first_submit =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));
  ASSERT_EQ(first_submit.status_code, 200);
  const std::string first_job_id = first_submit.body.at("payload").at("job_id").get<std::string>();

  json other_user_request = MinimalJobRequest();
  other_user_request["user_id"] = "user-bob";
  other_user_request["workspace_id"] = "workspace-other";
  const pafio::platform::HttpResponse other_submit =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs", other_user_request));
  ASSERT_EQ(other_submit.status_code, 200);

  json second_workspace_request = MinimalJobRequest();
  second_workspace_request["workspace_id"] = "workspace-feature";
  const pafio::platform::HttpResponse second_submit =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs", second_workspace_request));
  ASSERT_EQ(second_submit.status_code, 200);
  const std::string second_job_id = second_submit.body.at("payload").at("job_id").get<std::string>();

  const json claim_body = {
      {"worker_id", "worker-01"},
      {"region", "local-dev"},
      {"worker_pool_key", "default"},
      {"compile_container_id", "container-01"},
  };

  const pafio::platform::HttpResponse first_claim =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs/claim", claim_body));
  ASSERT_EQ(first_claim.status_code, 200);
  ASSERT_TRUE(first_claim.body.at("payload").at("claimed").get<bool>());
  EXPECT_EQ(first_claim.body.at("payload").at("job").at("job_id").get<std::string>(), first_job_id);
  EXPECT_EQ(first_claim.body.at("payload").at("compile_container").at("workspace_generation").get<int>(), 1);

  const pafio::platform::HttpResponse first_complete =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/jobs/" + first_job_id + "/complete",
          {
              {"worker_id", "worker-01"},
              {"status", "succeeded"},
              {"message", "build completed"},
          }));
  ASSERT_EQ(first_complete.status_code, 200);

  const pafio::platform::HttpResponse second_claim =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs/claim", claim_body));
  ASSERT_EQ(second_claim.status_code, 200);
  ASSERT_TRUE(second_claim.body.at("payload").at("claimed").get<bool>());
  EXPECT_EQ(second_claim.body.at("payload").at("job").at("job_id").get<std::string>(), second_job_id);
  EXPECT_EQ(second_claim.body.at("payload").at("compile_container").at("user_id").get<std::string>(), "user-alice");
  EXPECT_EQ(second_claim.body.at("payload").at("compile_container").at("current_workspace_id").get<std::string>(), "workspace-feature");
  EXPECT_EQ(second_claim.body.at("payload").at("compile_container").at("workspace_generation").get<int>(), 2);

  const pafio::platform::HttpResponse switched_events =
      router.Dispatch(Request(pafio::platform::HttpMethod::Get, "/jobs/" + second_job_id + "/events"));
  ASSERT_EQ(switched_events.status_code, 200);
  ASSERT_GE(switched_events.body.at("payload").at("events").size(), 2U);
  EXPECT_EQ(switched_events.body.at("payload").at("events").at(0).at("message").get<std::string>(), "job queued");
  EXPECT_EQ(switched_events.body.at("payload").at("events").at(1).at("message").get<std::string>(), "compile container switched workspace");

  const pafio::platform::HttpResponse wrong_user_switch =
      router.Dispatch(Request(
          pafio::platform::HttpMethod::Post,
          "/compile-containers/container-01/switch-workspace",
          {
              {"worker_id", "worker-01"},
              {"tenant_id", "tenant-acme"},
              {"user_id", "user-bob"},
              {"workspace_id", "workspace-other"},
          }));
  EXPECT_EQ(wrong_user_switch.status_code, 403);

  const pafio::platform::HttpResponse no_more_matching_work =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs/claim", claim_body));
  ASSERT_EQ(no_more_matching_work.status_code, 200);
  EXPECT_FALSE(no_more_matching_work.body.at("payload").at("claimed").get<bool>());
}

TEST(PlatformServiceJobQueueTests, RepeatedSubmissionsUseMonotonicIdsAndMissingMutationsDoNotCreateJobs)
{
  pafio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.object_store.provider = "memory";
  config.mtls.required = true;

  pafio::platform::PlatformRouter router(config);

  const pafio::platform::HttpResponse first =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));
  const pafio::platform::HttpResponse second =
      router.Dispatch(Request(pafio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));

  ASSERT_EQ(first.status_code, 200);
  ASSERT_EQ(second.status_code, 200);
  EXPECT_EQ(first.body.at("payload").at("job_id").get<std::string>(), "job-000000000001");
  EXPECT_EQ(second.body.at("payload").at("job_id").get<std::string>(), "job-000000000002");

  const pafio::platform::HttpResponse cancel_missing = router.Dispatch(Request(
      pafio::platform::HttpMethod::Post,
      "/jobs/job-000000999999/cancel",
      {{"reason", "missing"}}));
  EXPECT_EQ(cancel_missing.status_code, 404);

  const pafio::platform::HttpResponse heartbeat_missing = router.Dispatch(Request(
      pafio::platform::HttpMethod::Post,
      "/jobs/job-000000999999/heartbeat",
      {{"worker_id", "worker-01"}}));
  EXPECT_EQ(heartbeat_missing.status_code, 404);

  const pafio::platform::HttpResponse complete_missing = router.Dispatch(Request(
      pafio::platform::HttpMethod::Post,
      "/jobs/job-000000999999/complete",
      {{"worker_id", "worker-01"}, {"status", "succeeded"}}));
  EXPECT_EQ(complete_missing.status_code, 404);

  const pafio::platform::HttpResponse lookup_missing =
      router.Dispatch(Request(pafio::platform::HttpMethod::Get, "/jobs/job-000000999999"));
  EXPECT_EQ(lookup_missing.status_code, 404);
}

TEST(PlatformServiceWorkgroupTests, RegistersAndListsClustersWithDefaultPolicy)
{
  const fs::path root = MakeTempDir("platform-workgroup-register");
  pafio::platform::PlatformConfig config = TestPlatformConfig(root);
  config.workgroup.registration_token = "local-token";

  pafio::platform::PlatformRouter router(config);
  const json registration = {
      {"cluster_id", "dev-a"},
      {"region", "local-dev"},
      {"node_id", "primary-0"},
      {"control_plane_endpoint", "http://127.0.0.1:8787/api/styio-platform/v1"},
      {"internal_control_plane_endpoint", "http://styio-styio-platform-primary.styio-platform-dev.svc.cluster.local:8787/api/styio-platform/v1"},
      {"registry_endpoint", "http://127.0.0.1:8080"},
      {"roles", json::array({"control-plane", "worker", "mirror", "registry-writer"})},
      {"trust_domain", "styio-platform-local"},
      {"registration_token", "local-token"},
      {"labels", {{"source", "dev-env"}, {"namespace", "styio-platform-dev"}}},
  };

  const pafio::platform::HttpResponse registered = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/workgroups/local-dev/clusters/register",
      OperatorIdentity(),
      registration));
  ASSERT_EQ(registered.status_code, 200);
  EXPECT_EQ(registered.body.at("message").get<std::string>(), "registered workgroup cluster");
  const json cluster = registered.body.at("payload");
  EXPECT_EQ(cluster.at("workgroup_id").get<std::string>(), "local-dev");
  EXPECT_EQ(cluster.at("cluster_id").get<std::string>(), "dev-a");
  EXPECT_EQ(cluster.at("registration_policy").get<std::string>(), "local-dev-default");
  EXPECT_EQ(cluster.at("registered_by").at("role").get<std::string>(), "operator");
  EXPECT_FALSE(cluster.contains("registration_token"));

  const pafio::platform::HttpResponse listed = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/workgroups/local-dev/clusters",
      OperatorIdentity()));
  ASSERT_EQ(listed.status_code, 200);
  EXPECT_EQ(listed.body.at("payload").at("workgroup_id").get<std::string>(), "local-dev");
  EXPECT_TRUE(listed.body.at("payload").at("policy").at("registration_token_required").get<bool>());
  ASSERT_EQ(listed.body.at("payload").at("clusters").size(), 1U);
  EXPECT_EQ(listed.body.at("payload").at("clusters").at(0).at("cluster_id").get<std::string>(), "dev-a");
}

TEST(PlatformServiceWorkgroupTests, RejectsUnauthorizedOrInvalidClusterRegistration)
{
  const fs::path root = MakeTempDir("platform-workgroup-deny");
  pafio::platform::PlatformConfig config = TestPlatformConfig(root);
  config.workgroup.registration_token = "local-token";

  pafio::platform::PlatformRouter router(config);
  const json registration = {
      {"cluster_id", "dev-a"},
      {"region", "local-dev"},
      {"node_id", "primary-0"},
      {"control_plane_endpoint", "http://127.0.0.1:8787/api/styio-platform/v1"},
      {"registration_token", "wrong-token"},
  };

  const pafio::platform::HttpResponse worker_denied = router.Dispatch(Request(
      pafio::platform::HttpMethod::Post,
      "/workgroups/local-dev/clusters/register",
      registration));
  ASSERT_EQ(worker_denied.status_code, 403);
  EXPECT_EQ(worker_denied.body.at("error_payload").at("category").get<std::string>(), "AuthError");

  const pafio::platform::HttpResponse token_denied = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/workgroups/local-dev/clusters/register",
      OperatorIdentity(),
      registration));
  ASSERT_EQ(token_denied.status_code, 403);
  EXPECT_EQ(token_denied.body.at("error_payload").at("detail").get<std::string>(), "registration token is invalid");
}

TEST(PlatformEcosystemManagementTests, ListsRepositoriesAndPlansStableRelease)
{
  const fs::path root = MakeTempDir("platform-ecosystem-management");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);
  pafio::platform::PlatformRouter router(config);

  const pafio::platform::HttpResponse list = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/ecosystem/repositories",
      OperatorIdentity()));
  ASSERT_EQ(list.status_code, 200);
  const json repositories = list.body.at("payload").at("repositories");
  ASSERT_EQ(repositories.size(), 5U);
  EXPECT_EQ(repositories.at(0).at("id").get<std::string>(), "styio");
  EXPECT_EQ(repositories.at(1).at("id").get<std::string>(), "pafio-nightly");
  EXPECT_EQ(
      repositories.at(1).at("artifacts").at(0).get<std::string>(),
      "pafio-cli");
  EXPECT_EQ(repositories.at(3).at("id").get<std::string>(), "styio-platform");
  EXPECT_EQ(repositories.at(3).at("branch").get<std::string>(), "stable");
  EXPECT_EQ(repositories.at(3).at("runtime").at("adapter").get<std::string>(), "cmake-server");

  const pafio::platform::HttpResponse plan = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/ecosystem/releases/plan",
      OperatorIdentity(),
      {
          {"release_id", "v0.1.0"},
          {"version", "v0.1.0"},
          {"components", {{"vityo-nightly", "v0.1.1-vityo"}}},
      }));
  ASSERT_EQ(plan.status_code, 200);
  EXPECT_EQ(plan.body.at("payload").at("branch").get<std::string>(), "stable");
  ASSERT_EQ(plan.body.at("payload").at("execution_plan").size(), 5U);
  EXPECT_EQ(plan.body.at("payload").at("execution_plan").at(2).at("repository_id").get<std::string>(), "vityo-nightly");
  EXPECT_EQ(plan.body.at("payload").at("execution_plan").at(2).at("fetch").at("ref").get<std::string>(), "v0.1.1-vityo");
  EXPECT_EQ(plan.body.at("payload").at("execution_plan").at(3).at("fetch").at("ref").get<std::string>(), "v0.1.0");

  const pafio::platform::HttpResponse invalid = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/ecosystem/releases/plan",
      OperatorIdentity(),
      {{"components", {{"unknown", "v0.1.0"}}}}));
  ASSERT_EQ(invalid.status_code, 400);
  EXPECT_EQ(invalid.body.at("error_payload").at("operation_id").get<std::string>(), "planEcosystemRelease");
}

TEST(PlatformDocumentationGovernanceTests, ListsGovernanceAndPlansDocumentationChange)
{
  const fs::path root = MakeTempDir("platform-documentation-governance");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);
  pafio::platform::PlatformRouter router(config);

  const pafio::platform::HttpResponse list = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/docs/governance",
      OperatorIdentity()));
  ASSERT_EQ(list.status_code, 200);
  const json payload = list.body.at("payload");
  EXPECT_EQ(payload.at("governance_id").get<std::string>(), "styio-docs");
  ASSERT_GE(payload.at("collections").size(), 5U);
  EXPECT_EQ(payload.at("branch_policy").at("single_branch").get<std::string>(), "stable");

  const pafio::platform::HttpResponse plan = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/docs/change-plan",
      OperatorIdentity(),
      {
          {"repository_id", "styio-platform"},
          {"change_kind", "control-plane-contract"},
          {"changed_paths",
           {
               "contracts/platform-control-plane/v1/platform-control-plane.contract.json",
               "docs/governance/Platform-Documentation-Governance.md",
               "src/PlatformCloud/DocumentationGovernance/DocumentationGovernance.cpp",
           }},
      }));
  ASSERT_EQ(plan.status_code, 200);
  const json required_gates = plan.body.at("payload").at("required_gates");
  EXPECT_NE(std::find(required_gates.begin(), required_gates.end(), "contract-gate"), required_gates.end());
  EXPECT_NE(std::find(required_gates.begin(), required_gates.end(), "team-docs-gate"), required_gates.end());
  const json required_runbooks = plan.body.at("payload").at("required_runbooks");
  EXPECT_NE(
      std::find(required_runbooks.begin(), required_runbooks.end(), "docs/teams/CONTROL-PLANE-RUNBOOK.md"),
      required_runbooks.end());
  EXPECT_NE(
      std::find(required_runbooks.begin(), required_runbooks.end(), "docs/teams/PLATFORM-KERNEL-RUNBOOK.md"),
      required_runbooks.end());
  EXPECT_NE(
      std::find(required_runbooks.begin(), required_runbooks.end(), "docs/teams/DOC-STATS.md"),
      required_runbooks.end());

  const pafio::platform::HttpResponse invalid = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/docs/change-plan",
      OperatorIdentity(),
      {{"changed_paths", {"/absolute/path"}}}));
  ASSERT_EQ(invalid.status_code, 400);
  EXPECT_EQ(invalid.body.at("error_payload").at("operation_id").get<std::string>(), "planDocumentationChange");
}

TEST(PlatformProductionOpsTests, RecoveryOpsReleaseChannelsStorageAndExternalIdentityApisWork)
{
  const fs::path root = MakeTempDir("platform-production-ops");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);
  WriteFile(fs::path(config.registry.root) / "config.json", "{\"registry\":\"test\"}\n");
  WriteFile(fs::path(config.registry.root) / "_publications/pub-000001/publication.json", "{\"publication_id\":\"pub-000001\"}\n");

  pafio::platform::PlatformRouter router(config);
  const pafio::platform::HttpResponse snapshot = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/ops/recovery/snapshots",
      OperatorIdentity(),
      {{"snapshot_id", "snap-000001"}, {"label", "before-rollout"}}));
  ASSERT_EQ(snapshot.status_code, 200);
  EXPECT_EQ(snapshot.body.at("payload").at("snapshot_id").get<std::string>(), "snap-000001");

  WriteFile(fs::path(config.registry.root) / "config.json", "{\"registry\":\"changed\"}\n");
  const pafio::platform::HttpResponse restore = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/ops/recovery/snapshots/snap-000001/restore",
      OperatorIdentity()));
  ASSERT_EQ(restore.status_code, 200);
  EXPECT_TRUE(restore.body.at("payload").at("verified").get<bool>());
  EXPECT_NE(ReadFile(fs::path(config.registry.root) / "config.json").find("\"test\""), std::string::npos);

  const pafio::platform::HttpResponse rollout = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/release-channels/canary/rollout",
      OperatorIdentity(),
      {{"publication_id", "pub-000001"}, {"percentage", 10}, {"ring", "internal"}}));
  ASSERT_EQ(rollout.status_code, 200);
  EXPECT_EQ(rollout.body.at("payload").at("channel").get<std::string>(), "canary");
  EXPECT_EQ(rollout.body.at("payload").at("percentage").get<int>(), 10);

  const pafio::platform::HttpResponse storage = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/storage/status",
      OperatorIdentity()));
  ASSERT_EQ(storage.status_code, 200);
  EXPECT_EQ(storage.body.at("payload").at("object_store_provider").get<std::string>(), "memory");

  const pafio::platform::HttpResponse audit = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/ops/audit-events",
      OperatorIdentity()));
  ASSERT_EQ(audit.status_code, 200);
  EXPECT_GE(audit.body.at("payload").at("events").size(), 2U);

  const pafio::platform::HttpResponse metrics = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/ops/metrics",
      OperatorIdentity()));
  ASSERT_EQ(metrics.status_code, 200);
  EXPECT_GE(metrics.body.at("payload").at("requests").at("total_requests").get<int>(), 4);

  const pafio::platform::HttpResponse external = router.Dispatch({
      .method = pafio::platform::HttpMethod::Post,
      .path = "/identity/external/exchange",
      .body = {
          {"provider", "microsoft"},
          {"tenant_id", "tenant-acme"},
          {"roles", json::array({"developer"})},
          {"claims", {{"sub", "aad-user-01"}, {"email", "alice@example.test"}}},
      },
  });
  ASSERT_EQ(external.status_code, 200);
  EXPECT_EQ(external.body.at("payload").at("actor_id").get<std::string>(), "external:microsoft:aad-user-01");
}

TEST(PlatformRegistryControlPlaneTests, StatusUsesRedactedPathsAndFilesystemReadiness)
{
  const fs::path root = MakeTempDir("platform-registry-status");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);
  WriteFile(fs::path(config.registry.root) / "config.json", "{}\n");
  WriteFile(fs::path(config.registry.root) / "trust/root.json", "{}\n");
  fs::create_directories(config.registry.key_dir);

  pafio::platform::PlatformRouter router(config);
  const pafio::platform::HttpResponse status = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/status",
      RegistryWriterIdentity()));

  ASSERT_EQ(status.status_code, 200);
  ASSERT_EQ(status.body.at("returncode").get<int>(), 0);
  const json payload = status.body.at("payload");
  EXPECT_EQ(payload.at("registry_root").get<std::string>(), "<redacted>");
  EXPECT_EQ(payload.at("key_dir").get<std::string>(), "<redacted>");
  EXPECT_EQ(payload.at("registry_name").get<std::string>(), "test-registry");
  EXPECT_TRUE(payload.at("root_initialized").get<bool>());
  EXPECT_TRUE(payload.at("config_present").get<bool>());
  EXPECT_TRUE(payload.at("root_metadata_present").get<bool>());
  EXPECT_EQ(payload.at("publish_endpoint").get<std::string>(), "/api/pafio-registry-control/v1/publish");
  EXPECT_EQ(payload.at("verify_endpoint").get<std::string>(), "/api/pafio-registry-control/v1/verify");
  EXPECT_EQ(payload.at("descriptor_endpoint").get<std::string>(), "/api/pafio-registry-control/v1/descriptor");
  EXPECT_EQ(status.body.dump().find(config.registry.root), std::string::npos);
  EXPECT_EQ(status.body.dump().find(config.registry.key_dir), std::string::npos);
}

TEST(PlatformRegistryControlPlaneTests, PublishVerifyAndMirrorStatusUseLocalState)
{
  const fs::path root = MakeTempDir("platform-registry-publish");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);

  pafio::platform::PlatformRouter router(config);
  const json publish_request = PafioPublishRequest(
      "demo/app",
      "0.1.0",
      json::array({
          {
              {"alias", "zeta"},
              {"package", "demo/zeta"},
              {"version_req", "2.0.0"},
              {"registry", "https://packages.example.test"},
          },
          {
              {"alias", "alpha"},
              {"package", "demo/alpha"},
              {"version_req", "1.0.0"},
              {"registry", "https://packages.example.test"},
          },
      }),
      json::array({
          {
              {"alias", "fixture"},
              {"package", "demo/fixture"},
              {"version_req", "3.0.0"},
              {"registry", "https://packages.example.test"},
          },
      }));
  const pafio::platform::HttpResponse publish = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      publish_request));

  ASSERT_EQ(publish.status_code, 200);
  ASSERT_EQ(publish.body.at("returncode").get<int>(), 0);
  const json published = publish.body.at("payload");
  EXPECT_EQ(published.at("package").get<std::string>(), "demo/app");
  EXPECT_EQ(published.at("version").get<std::string>(), "0.1.0");
  EXPECT_EQ(published.at("publisher_id").get<std::string>(), "registry-writer-01");
  EXPECT_EQ(published.at("archive_name").get<std::string>(), "app-0.1.0.pafio.src.tar");
  EXPECT_TRUE(published.at("created_root").get<bool>());
  EXPECT_EQ(published.at("sequence").get<int>(), 1);
  EXPECT_EQ(published.at("archive_sha256").get<std::string>().size(), 64U);
  EXPECT_FALSE(published.contains("registry_root"));
  EXPECT_FALSE(published.contains("registry_read_root"));
  EXPECT_FALSE(published.contains("archive_path"));
  EXPECT_EQ(publish.body.dump().find(config.registry.root), std::string::npos);
  EXPECT_TRUE(published.at("artifact_path").get<std::string>().ends_with(".pafio.src.tar"));
  ASSERT_EQ(published.at("dependencies").size(), 2U);
  EXPECT_EQ(published.at("dependencies").at(0).at("alias").get<std::string>(), "alpha");
  ASSERT_EQ(published.at("dev_dependencies").size(), 1U);
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / published.at("artifact_path").get<std::string>()));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / published.at("index_path").get<std::string>()));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / published.at("log_leaf_path").get<std::string>()));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "trust/timestamp.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "trust/snapshot.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "trust/targets/demo.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "log/checkpoint.json"));
  EXPECT_FALSE(DirectoryHasEntries(fs::path(config.registry.root) / "_staging/uploads"));

  const json index_record = json::parse(ReadFile(
      fs::path(config.registry.root) / published.at("index_path").get<std::string>()));
  ASSERT_EQ(index_record.at("dependencies").size(), 2U);
  EXPECT_EQ(index_record.at("dependencies").at(0).at("alias").get<std::string>(), "alpha");
  EXPECT_EQ(index_record.at("dependencies").at(0).at("kind").get<std::string>(), "runtime");
  ASSERT_EQ(index_record.at("dev_dependencies").size(), 1U);
  EXPECT_EQ(index_record.at("dev_dependencies").at(0).at("kind").get<std::string>(), "development");

  const json registry_config = json::parse(ReadFile(fs::path(config.registry.root) / "config.json"));
  EXPECT_EQ(registry_config.at("protocol").get<std::string>(), "pafio-static-registry");
  EXPECT_EQ(registry_config.at("protocol_version").get<int>(), 2);
  const json root_metadata = json::parse(ReadFile(fs::path(config.registry.root) / "trust/root.json"));
  EXPECT_EQ(root_metadata.at("signed").at("type").get<std::string>(), "root");
  ASSERT_FALSE(root_metadata.at("signatures").empty());

  const pafio::platform::HttpResponse descriptor = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/descriptor",
      RegistryWriterIdentity()));
  ASSERT_EQ(descriptor.status_code, 200);
  ASSERT_EQ(descriptor.body.at("returncode").get<int>(), 0);
  const json descriptor_payload = descriptor.body.at("payload");
  EXPECT_EQ(descriptor_payload.at("schema_version").get<int>(), 1);
  EXPECT_EQ(descriptor_payload.at("registry_name").get<std::string>(), "test-registry");
  EXPECT_EQ(descriptor_payload.at("root_sha256").get<std::string>().size(), 64U);
  EXPECT_EQ(descriptor_payload.at("control_plane_base_url").get<std::string>(), "/api/pafio-registry-control/v1");
  EXPECT_EQ(descriptor_payload.at("descriptor_signature").get<std::string>(), "platform-control-plane-mtls");

  const pafio::platform::HttpResponse verify = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/verify",
      MirrorIdentity(),
      json::object()));
  ASSERT_EQ(verify.status_code, 200);
  EXPECT_TRUE(verify.body.at("payload").at("ok").get<bool>());
  EXPECT_EQ(verify.body.at("payload").at("namespaces").get<int>(), 1);
  EXPECT_EQ(verify.body.at("payload").at("index_files").get<int>(), 1);
  EXPECT_EQ(verify.body.at("payload").at("releases").get<int>(), 1);
  EXPECT_EQ(verify.body.at("payload").at("tree_size").get<int>(), 1);

  const pafio::platform::HttpResponse mirror = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/mirrors/mirror-local/status",
      MirrorIdentity()));
  ASSERT_EQ(mirror.status_code, 200);
  EXPECT_EQ(mirror.body.at("payload").at("freshness").get<std::string>(), "fresh");
  EXPECT_EQ(mirror.body.at("payload").at("replay_cursor").get<std::string>(), "checkpoint-0001");
  EXPECT_EQ(mirror.body.at("payload").at("publication_id").get<std::string>(), "pub-000001");
  EXPECT_EQ(mirror.body.at("payload").at("repository_version_id").get<std::string>(), "rv-000001");
}

TEST(PlatformRegistryControlPlaneTests, PackageRepositoryPublicationOwnerTokenAndYankApisWork)
{
  const fs::path root = MakeTempDir("platform-registry-domain");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);

  pafio::platform::PlatformRouter router(config);
  const json publish_request = PafioPublishRequest();
  const pafio::platform::HttpResponse publish = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      publish_request));
  ASSERT_EQ(publish.status_code, 200);
  EXPECT_EQ(publish.body.at("payload").at("publication_id").get<std::string>(), "pub-000001");
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "_publications/pub-000001/publication.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "_distributions/default/current.json"));

  const pafio::platform::HttpResponse package = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/packages/demo/app",
      RegistryWriterIdentity()));
  ASSERT_EQ(package.status_code, 200);
  EXPECT_EQ(package.body.at("payload").at("latest_version").get<std::string>(), "0.1.0");

  const pafio::platform::HttpResponse release = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/packages/demo/app/releases/0.1.0",
      RegistryWriterIdentity()));
  ASSERT_EQ(release.status_code, 200);
  EXPECT_FALSE(release.body.at("payload").at("yanked").get<bool>());

  const pafio::platform::HttpResponse add_owner = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/packages/demo/app/owners",
      RegistryWriterIdentity(),
      {{"owner_id", "user-bob"}, {"owner_kind", "user"}, {"role", "owner"}}));
  ASSERT_EQ(add_owner.status_code, 200);
  const pafio::platform::HttpResponse owners = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/packages/demo/app/owners",
      RegistryWriterIdentity()));
  ASSERT_EQ(owners.status_code, 200);
  EXPECT_EQ(owners.body.at("payload").at("owners").size(), 2U);
  EXPECT_EQ(
      owners.body.at("payload").at("owners").at(0).at("owner_id").get<std::string>(),
      "registry-writer-01");
  const pafio::platform::HttpResponse remove_owner = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Delete,
      "/api/pafio-registry-control/v1/packages/demo/app/owners/user-bob",
      RegistryWriterIdentity()));
  ASSERT_EQ(remove_owner.status_code, 200);

  const pafio::platform::HttpResponse token_created = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/tokens",
      RegistryWriterIdentity(),
      {
          {"scopes", json::array({"package:publish", "package:yank", "package:owner", "repository:promote"})},
          {"package_patterns", json::array({"demo/*"})},
      }));
  ASSERT_EQ(token_created.status_code, 200);
  const std::string token = token_created.body.at("payload").at("token").get<std::string>();
  EXPECT_FALSE(token_created.body.at("payload").contains("token_hash"));

  const pafio::platform::HttpResponse denied_publish = router.Dispatch(RequestWithToken(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/publish",
      token,
      PafioPublishRequest("other/app", "1.0.0")));
  ASSERT_EQ(denied_publish.status_code, 403);
  EXPECT_FALSE(DirectoryHasEntries(fs::path(config.registry.root) / "_staging/uploads"));

  const pafio::platform::HttpResponse yank = router.Dispatch(RequestWithToken(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/packages/demo/app/releases/0.1.0/yank",
      token,
      {{"reason", "bad metadata"}}));
  ASSERT_EQ(yank.status_code, 200);
  EXPECT_TRUE(yank.body.at("payload").at("yanked").get<bool>());
  EXPECT_TRUE(yank.body.at("payload").at("artifact_preserved").get<bool>());
  EXPECT_EQ(yank.body.at("payload").at("publication_id").get<std::string>(), "pub-000002");

  const pafio::platform::HttpResponse yanked_release = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/packages/demo/app/releases/0.1.0",
      RegistryWriterIdentity()));
  ASSERT_EQ(yanked_release.status_code, 200);
  EXPECT_TRUE(yanked_release.body.at("payload").at("yanked").get<bool>());

  const pafio::platform::HttpResponse versions = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/repositories/default/versions",
      RegistryWriterIdentity()));
  ASSERT_EQ(versions.status_code, 200);
  EXPECT_EQ(versions.body.at("payload").at("versions").size(), 2U);

  const pafio::platform::HttpResponse publication = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/publications/pub-000002",
      RegistryWriterIdentity()));
  ASSERT_EQ(publication.status_code, 200);
  EXPECT_TRUE(publication.body.at("payload").at("verified").get<bool>());

  const pafio::platform::HttpResponse rollback = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/distributions/default/rollback",
      OperatorIdentity(),
      json::object()));
  ASSERT_EQ(rollback.status_code, 200);
  EXPECT_EQ(rollback.body.at("payload").at("publication_id").get<std::string>(), "pub-000001");

  const pafio::platform::HttpResponse promote = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/distributions/default/promote",
      OperatorIdentity(),
      {{"publication_id", "pub-000002"}}));
  ASSERT_EQ(promote.status_code, 200);
  EXPECT_EQ(promote.body.at("payload").at("publication_id").get<std::string>(), "pub-000002");

  const pafio::platform::HttpResponse unyank = router.Dispatch(RequestWithToken(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/packages/demo/app/releases/0.1.0/unyank",
      token,
      json::object()));
  ASSERT_EQ(unyank.status_code, 200);
  EXPECT_FALSE(unyank.body.at("payload").at("yanked").get<bool>());
  EXPECT_TRUE(unyank.body.at("payload").at("artifact_preserved").get<bool>());
  EXPECT_EQ(unyank.body.at("payload").at("publication_id").get<std::string>(), "pub-000003");

  const pafio::platform::HttpResponse final_release = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Get,
      "/api/pafio-registry-control/v1/packages/demo/app/releases/0.1.0",
      RegistryWriterIdentity()));
  ASSERT_EQ(final_release.status_code, 200);
  EXPECT_FALSE(final_release.body.at("payload").at("yanked").get<bool>());

  const pafio::platform::HttpResponse revoked = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Delete,
      "/api/pafio-registry-control/v1/tokens/" + token_created.body.at("payload").at("token_id").get<std::string>(),
      RegistryWriterIdentity()));
  ASSERT_EQ(revoked.status_code, 200);

  const pafio::platform::HttpResponse revoked_token_denied = router.Dispatch(RequestWithToken(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/packages/demo/app/releases/0.1.0/yank",
      token,
      json::object()));
  ASSERT_EQ(revoked_token_denied.status_code, 403);

  pafio::platform::PlatformConfig mirror_config = config;
  mirror_config.registry.root = (root / "mirror-registry").string();
  mirror_config.registry.mirror_source_root = config.registry.root;
  ASSERT_EQ(pafio::platform::RunMirrorSyncOnce(mirror_config), 0);
  const json mirror_current = json::parse(ReadFile(fs::path(mirror_config.registry.root) / "_distributions/default/current.json"));
  EXPECT_EQ(mirror_current.at("publication_id").get<std::string>(), "pub-000003");
  EXPECT_TRUE(fs::exists(fs::path(mirror_config.registry.root) / "_publications/pub-000003/publication.json"));
}

TEST(PlatformRegistryControlPlaneTests, RejectsMalformedPafioArchiveUploadsWithoutStagingResidue)
{
  const fs::path root = MakeTempDir("platform-registry-invalid-upload");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);
  pafio::platform::PlatformRouter router(config);

  std::vector<json> malformed_requests;
  json unexpected_field = PafioPublishRequest();
  unexpected_field["manifest_path"] = "pafio.toml";
  malformed_requests.push_back(std::move(unexpected_field));

  json missing_field = PafioPublishRequest();
  missing_field.erase("dependencies");
  malformed_requests.push_back(std::move(missing_field));

  json noncanonical_base64 = PafioPublishRequest();
  noncanonical_base64["archive_base64"] = "YXJjaGl2ZQ";
  malformed_requests.push_back(std::move(noncanonical_base64));

  json wrong_size = PafioPublishRequest();
  wrong_size["archive_size_bytes"] = 8;
  malformed_requests.push_back(std::move(wrong_size));

  json unsafe_name = PafioPublishRequest();
  unsafe_name["archive_name"] = "../app-0.1.0.pafio.src.tar";
  malformed_requests.push_back(std::move(unsafe_name));

  json wrong_digest = PafioPublishRequest();
  wrong_digest["archive_sha256"] = std::string(64U, '0');
  malformed_requests.push_back(std::move(wrong_digest));

  std::string checksum_archive = BuildTestPafioArchive();
  checksum_archive[0] = checksum_archive[0] == 'x' ? 'y' : 'x';
  malformed_requests.push_back(PafioPublishRequestForArchive(
      std::move(checksum_archive),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  const std::string prefix = "app-0.1.0";
  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestCanonicalUstar({
          {prefix + "/../pafio.toml", TestPafioManifest("demo/app", "0.1.0")},
      }),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  std::string truncated_trailer = BuildTestPafioArchive();
  truncated_trailer.resize(truncated_trailer.size() - 512U);
  malformed_requests.push_back(PafioPublishRequestForArchive(
      std::move(truncated_trailer),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchive("demo/app", "0.1.0", json::array(), json::array(), false),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchiveWithManifest(
          TestPafioManifest("demo/other", "0.1.0")),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchiveWithManifest(
          TestPafioManifest("demo/app", "9.9.9")),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchiveWithManifest(
          TestPafioManifest("demo/app", "0.1")),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  const json expected_dependencies = json::array({
      {
          {"alias", "base"},
          {"package", "demo/base"},
          {"version_req", "1.0.0"},
          {"registry", "https://packages.example.test"},
      },
  });
  const json invalid_manifest_dependency_version = json::array({
      {
          {"alias", "base"},
          {"package", "demo/base"},
          {"version_req", "^1.0.0"},
          {"registry", "https://packages.example.test"},
      },
  });
  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchive(
          "demo/app",
          "0.1.0",
          invalid_manifest_dependency_version),
      "demo/app",
      "0.1.0",
      expected_dependencies,
      json::array()));

  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchive(),
      "demo/app",
      "0.1.0",
      expected_dependencies,
      json::array()));

  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchiveWithManifest(
          "[pafio]\nmanifest-version = 1\n\n" +
          TestPafioManifest("demo/app", "0.1.0")),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  malformed_requests.push_back(PafioPublishRequestForArchive(
      BuildTestPafioArchiveWithManifest(
          TestPafioManifest("demo/app", "0.1.0") +
          "\n[dependencies]\n"
          "base = { package = \"demo/base\", path = \"../base\" }\n"),
      "demo/app",
      "0.1.0",
      json::array(),
      json::array()));

  for (const json &request : malformed_requests) {
    const pafio::platform::HttpResponse response = router.Dispatch(RequestWithIdentity(
        pafio::platform::HttpMethod::Post,
        "/api/pafio-registry-control/v1/publish",
        RegistryWriterIdentity(),
        request));
    EXPECT_EQ(response.status_code, 400);
    EXPECT_EQ(response.body.at("error_payload").at("category").get<std::string>(), "UsageError");
    EXPECT_FALSE(DirectoryHasEntries(fs::path(config.registry.root) / "_staging/uploads"));
    EXPECT_FALSE(fs::exists(fs::path(config.registry.root) / "config.json"));
    EXPECT_FALSE(fs::exists(fs::path(config.registry.root) / "trust/root.json"));
    EXPECT_FALSE(fs::exists(fs::path(config.registry.root) / "index"));
    EXPECT_FALSE(fs::exists(fs::path(config.registry.root) / "_staging"));
    EXPECT_FALSE(fs::exists(config.registry.key_dir));
    EXPECT_FALSE(fs::exists(config.registry.root));
  }
}

TEST(PlatformRegistryControlPlaneTests, EnforcesRegistryRolesAndNonSuccessDomainErrors)
{
  const fs::path root = MakeTempDir("platform-registry-errors");
  const pafio::platform::PlatformConfig config = TestPlatformConfig(root);

  pafio::platform::PlatformRouter router(config);
  const json publish_request = PafioPublishRequest();

  const pafio::platform::HttpResponse denied = router.Dispatch(Request(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/publish",
      publish_request));
  ASSERT_EQ(denied.status_code, 403);
  EXPECT_EQ(denied.body.at("returncode").get<int>(), 2);

  const pafio::platform::HttpResponse verify_before_publish = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/verify",
      MirrorIdentity(),
      json::object()));
  ASSERT_EQ(verify_before_publish.status_code, 422);
  EXPECT_EQ(verify_before_publish.body.at("error_payload").at("category").get<std::string>(), "VerifyError");

  const pafio::platform::HttpResponse first_publish = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      publish_request));
  ASSERT_EQ(first_publish.status_code, 200);

  const pafio::platform::HttpResponse duplicate = router.Dispatch(RequestWithIdentity(
      pafio::platform::HttpMethod::Post,
      "/api/pafio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      publish_request));
  ASSERT_EQ(duplicate.status_code, 409);
  EXPECT_EQ(duplicate.body.at("returncode").get<int>(), 17);
  EXPECT_EQ(duplicate.body.at("error_payload").at("category").get<std::string>(), "PublishError");
  EXPECT_FALSE(duplicate.body.at("error_payload").contains("operation_id"));
  EXPECT_FALSE(DirectoryHasEntries(fs::path(config.registry.root) / "_staging/uploads"));
}
