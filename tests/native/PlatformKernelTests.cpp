#include "BuildTestSupport.hpp"

#include "SpioPlatformProtocols/Contract.hpp"
#include "SpioPlatformProtocols/Execution.hpp"
#include "SpioPlatformProtocols/Job.hpp"
#include "PlatformCore/Core/Errors.hpp"
#include "SpioPlatformProtocols/CompilePlan.hpp"
#include "PlatformCloud/DeveloperWorkspace/WorkerRuntimeFactory.hpp"
#include "PlatformCloud/PackageRegistry/MirrorSync/MirrorSync.hpp"
#include "PlatformService/Http.hpp"
#include "PlatformSecurity/PlatformCA/CertificateAuthority.hpp"
#include "PlatformSecurity/PlatformClientAuth/Authorization.hpp"
#include "PlatformSecurity/PlatformClientAuth/Identity.hpp"
#include "PlatformStorage/PlatformPersistence/ObjectStore.hpp"
#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"
#include "PlatformService/Router.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

using json = nlohmann::json;

using spio::testsupport::CanonicalAbsolutePath;
using spio::testsupport::MakeTempDir;
using spio::testsupport::ReadFile;
using spio::testsupport::WriteFile;

TEST(PlatformCompilePlanTests, WritesCompilePlanForSingleLibPackage)
{
  const fs::path root = MakeTempDir("platform-single-lib-plan");
  WriteFile(
      root / "spio.toml",
      "[spio]\n"
      "manifest-version = 1\n\n"
      "[package]\n"
      "name = \"acme/demo\"\n"
      "version = \"0.1.0\"\n"
      "edition = \"2026\"\n"
      "publish = false\n\n"
      "[toolchain]\n"
      "channel = \"nightly\"\n"
      "implicit-std = true\n\n"
      "[lib]\n"
      "path = \"src/lib.styio\"\n");
  WriteFile(root / "src/lib.styio", "# value := 1\n");

  const spio::BuildPlanResult result = spio::WriteBuildCompilePlan({
      .manifest_path = root / "spio.toml",
      .select_lib = true,
  });

  EXPECT_TRUE(fs::exists(result.plan_path));
  EXPECT_EQ(result.entry_target_kind, "lib");
  EXPECT_EQ(result.entry_package_name, "acme/demo");

  const json plan = json::parse(ReadFile(result.plan_path));
  EXPECT_EQ(plan["plan_version"], 1);
  EXPECT_EQ(plan["intent"], "build");
  EXPECT_EQ(plan["workspace_root"], CanonicalAbsolutePath(root).string());
  EXPECT_EQ(plan["entry"]["target_kind"], "lib");
  EXPECT_EQ(plan["entry"]["file"], CanonicalAbsolutePath(root / "src/lib.styio").string());
  EXPECT_EQ(plan["toolchain"]["std_package_id"], "builtin:std@nightly/2026");
  EXPECT_EQ(plan["profile"]["name"], "dev");
  EXPECT_EQ(plan["emit"]["error_format"], "jsonl");
  ASSERT_EQ(plan["packages"].size(), 1U);
  EXPECT_EQ(plan["packages"][0]["targets"]["lib"], CanonicalAbsolutePath(root / "src/lib.styio").string());
}

TEST(PlatformCompilePlanTests, RejectsMixedEditionGraphForCompilePlanV1)
{
  const fs::path root = MakeTempDir("platform-mixed-edition-plan");
  WriteFile(
      root / "spio.toml",
      "[spio]\n"
      "manifest-version = 1\n\n"
      "[package]\n"
      "name = \"acme/app\"\n"
      "version = \"0.1.0\"\n"
      "edition = \"2026\"\n"
      "publish = false\n\n"
      "[toolchain]\n"
      "channel = \"nightly\"\n"
      "implicit-std = true\n\n"
      "[[bin]]\n"
      "name = \"app\"\n"
      "path = \"src/main.styio\"\n\n"
      "[dependencies]\n"
      "util = { package = \"acme/util\", path = \"deps/util\" }\n");
  WriteFile(root / "src/main.styio", ">_(\"app\")\n");
  WriteFile(
      root / "deps/util/spio.toml",
      "[spio]\n"
      "manifest-version = 1\n\n"
      "[package]\n"
      "name = \"acme/util\"\n"
      "version = \"0.1.0\"\n"
      "edition = \"2027\"\n"
      "publish = false\n\n"
      "[toolchain]\n"
      "channel = \"nightly\"\n"
      "implicit-std = true\n\n"
      "[lib]\n"
      "path = \"src/lib.styio\"\n");
  WriteFile(root / "deps/util/src/lib.styio", "# util := 1\n");

  EXPECT_THROW(
      spio::WriteBuildCompilePlan({
          .manifest_path = root / "spio.toml",
      }),
      spio::PlanError);
}

TEST(PlatformCloudJobTests, EmitsControlPlaneBuildJobRequest)
{
  const spio::ProjectToolchainState state = {
      .manifest_path = "spio.toml",
      .state_path = "spio-toolchain.lock",
      .state_file_exists = true,
      .mode = "build",
      .channel = "nightly",
      .build_mode = "minimal",
      .risk_class = "trusted-internal",
      .preferred_execution_lane = "warm-shared",
      .security_profile = "trusted-warm",
      .source_revision = std::string("nightly-head"),
  };
  const spio::BuildPlanRequest request = {
      .manifest_path = "spio.toml",
      .package_name = std::string("acme/demo"),
      .bin_name = std::string("demo"),
      .profile = "dev",
      .build_mode = "minimal",
  };
  const spio::WorkflowInvocationOptions options = {
      .non_interactive = true,
      .source_revision = std::string("nightly-head"),
  };

  const spio::CloudExecutionPolicy policy = spio::ResolveCloudExecutionPolicy(state);
  const spio::CloudBuildJobRequest job = spio::BuildCloudBuildJobRequest("build", request, state, options, policy);
  const json payload = spio::BuildCloudBuildJobRequestPayload(job);

  EXPECT_EQ(payload.at("api_path").get<std::string>(), "/api/styio-platform/v1/jobs");
  EXPECT_EQ(payload.at("action").get<std::string>(), "build");
  EXPECT_EQ(payload.at("toolchain").at("mode").get<std::string>(), "build");
  EXPECT_EQ(payload.at("toolchain").at("build_mode").get<std::string>(), "minimal");
  EXPECT_EQ(payload.at("cloud").at("execution_lane").get<std::string>(), "warm-shared");
  EXPECT_EQ(payload.at("cloud").at("worker_pool_key").at("toolchain_mode").get<std::string>(), "build");
  EXPECT_TRUE(payload.at("cloud").at("cache_policy").at("worker_local_reuse").get<bool>());
  EXPECT_EQ(payload.at("source").at("requested_revision").get<std::string>(), "nightly-head");
  EXPECT_TRUE(payload.at("source").at("non_interactive").get<bool>());
}

TEST(PlatformCloudJobTests, RejectsSourceBuildOverridesWhenProjectUsesBinaryMode)
{
  const spio::ProjectToolchainState state = {
      .manifest_path = "spio.toml",
      .state_path = "spio-toolchain.lock",
      .state_file_exists = true,
      .mode = "binary",
      .channel = "stable",
      .build_mode = "minimal",
  };
  const spio::BuildPlanRequest request = {
      .manifest_path = "spio.toml",
      .intent = "build",
      .profile = "dev",
  };
  const spio::WorkflowInvocationOptions options = {
      .source_revision = std::string("nightly-head"),
  };

  try
  {
    (void) spio::BuildCloudBuildJobRequest("build", request, state, options, spio::ResolveCloudExecutionPolicy(state));
    FAIL() << "expected ValidationError";
  }
  catch (const spio::ValidationError &error)
  {
    EXPECT_EQ(std::string(error.what()), "source-build options require 'spio use build'");
  }
}

namespace
{

spio::platform::MtlsIdentity WorkerIdentity()
{
  return {
      .role = "worker",
      .tenant_id = "tenant-acme",
      .node_id = "worker-01",
  };
}

spio::platform::MtlsIdentity RegistryWriterIdentity()
{
  return {
      .role = "registry-writer",
      .tenant_id = "tenant-acme",
      .node_id = "registry-writer-01",
  };
}

spio::platform::MtlsIdentity MirrorIdentity()
{
  return {
      .role = "mirror",
      .tenant_id = "tenant-acme",
      .node_id = "mirror-01",
  };
}

spio::platform::MtlsIdentity OperatorIdentity()
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
      {"preferred_worker_pool", "linux/x86_64/build/nightly/minimal"},
      {"job_request", {
                          {"schema_version", 1},
                          {"api_path", "/api/styio-platform/v1/jobs"},
                          {"action", "build"},
                          {"manifest_path", "spio.toml"},
                          {"profile", "dev"},
                          {"source", {{"origin", "file:///tmp/styio-platform-test-workspace"}}},
                          {"toolchain", json::object()},
                          {"workflow", json::object()},
                          {"target", json::object()},
                          {"cloud", json::object()},
                      }},
  };
}

spio::platform::HttpRequest Request(
    spio::platform::HttpMethod method,
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

spio::platform::HttpRequest RequestWithIdentity(
    spio::platform::HttpMethod method,
    std::string path,
    spio::platform::MtlsIdentity identity,
    nlohmann::json body = nlohmann::json::object())
{
  return {
      .method = method,
      .path = std::move(path),
      .body = std::move(body),
      .identity = std::move(identity),
  };
}

spio::platform::HttpRequest RequestWithToken(
    spio::platform::HttpMethod method,
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

spio::platform::PlatformConfig TestPlatformConfig(const fs::path &root)
{
  spio::platform::PlatformConfig config;
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

}  // namespace

TEST(PlatformClientAuthTests, ParsesMtlsUriSanIntoRoleTenantAndNode)
{
  const std::optional<spio::platform::MtlsIdentity> identity =
      spio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/tenant-acme/role/worker/node/worker-01");

  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->role, "worker");
  EXPECT_EQ(identity->tenant_id, "tenant-acme");
  EXPECT_EQ(identity->node_id, "worker-01");
  EXPECT_TRUE(spio::platform::IsPlatformServiceRole(identity->role));

  const json serialized = spio::platform::SerializeMtlsIdentity(*identity);
  EXPECT_EQ(serialized.at("role").get<std::string>(), "worker");
  EXPECT_EQ(serialized.at("tenant_id").get<std::string>(), "tenant-acme");
  EXPECT_EQ(serialized.at("node_id").get<std::string>(), "worker-01");
}

TEST(PlatformClientAuthTests, RejectsUnknownMtlsUriSanRoleOrMissingNode)
{
  EXPECT_FALSE(spio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/acme/role/browser/node/client").has_value());
  EXPECT_FALSE(spio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/acme/role/worker").has_value());
  EXPECT_FALSE(spio::platform::ParseMtlsUriSan("https://styio-platform/tenant/acme/role/worker/node/worker-01").has_value());
}

TEST(PlatformClientAuthTests, AppliesOperationAuthorizationPolicy)
{
  const spio::platform::MtlsIdentity registry_writer{
      .role = "registry-writer",
      .tenant_id = "tenant-acme",
      .node_id = "registry-writer-01",
  };
  const spio::platform::MtlsIdentity worker{
      .role = "worker",
      .tenant_id = "tenant-acme",
      .node_id = "worker-01",
  };

  EXPECT_TRUE(spio::platform::IsInternalRole(worker));
  EXPECT_TRUE(spio::platform::IsAuthorizedForOperation("publishRelease", registry_writer));
  EXPECT_FALSE(spio::platform::IsAuthorizedForOperation("publishRelease", worker));
  EXPECT_FALSE(spio::platform::IsAuthorizedForOperation("registerWorkgroupCluster", registry_writer));
  EXPECT_TRUE(spio::platform::IsAuthorizedForOperation("claimJob", worker));
}

TEST(PlatformCATests, InitializesLocalCaAndIssuesMtlsCertificate)
{
  const fs::path root = MakeTempDir("platform-local-ca");
  spio::platform::PlatformCertificateAuthorityConfig config;
  config.root_dir = root / "mtls";
  config.ca_valid_days = 30;
  config.leaf_valid_days = 7;

  const spio::platform::PlatformCertificateSubject subject =
      spio::platform::BuildPlatformNodeCertificateSubject("worker", "tenant-acme", "worker-01");
  const spio::platform::PlatformCertificateBundle bundle =
      spio::platform::EnsurePlatformMtlsCertificate(config, subject);

  EXPECT_TRUE(fs::is_regular_file(bundle.ca_certificate_path));
  EXPECT_TRUE(fs::is_regular_file(bundle.ca_private_key_path));
  EXPECT_TRUE(fs::is_regular_file(bundle.certificate_path));
  EXPECT_TRUE(fs::is_regular_file(bundle.private_key_path));
  EXPECT_EQ(
      bundle.identity_uri_san,
      "spiffe://styio-platform/tenant/tenant-acme/role/worker/node/worker-01");

  const std::optional<spio::platform::MtlsIdentity> identity =
      spio::platform::ParseMtlsUriSan(bundle.identity_uri_san);
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->role, "worker");
  EXPECT_EQ(identity->tenant_id, "tenant-acme");
  EXPECT_EQ(identity->node_id, "worker-01");
  EXPECT_NE(ReadFile(bundle.ca_certificate_path).find("BEGIN CERTIFICATE"), std::string::npos);
  EXPECT_NE(ReadFile(bundle.private_key_path).find("BEGIN PRIVATE KEY"), std::string::npos);
}

TEST(PlatformServiceRouterTests, MatchesRouteParametersForJobsAndMirrors)
{
  const std::vector<spio::platform::RouteSpec> routes = spio::platform::BuildPlatformControlPlaneRoutes();

  const std::optional<spio::platform::RouteMatch> job =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Get, "/jobs/job-abc/events");
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->route.operation_id, "getJobEvents");
  EXPECT_EQ(job->parameters.at("job_id"), "job-abc");

  const std::optional<spio::platform::RouteMatch> mirror =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Get, "/mirrors/registry-primary/status");
  ASSERT_TRUE(mirror.has_value());
  EXPECT_EQ(mirror->route.operation_id, "mirrorStatus");
  EXPECT_EQ(mirror->parameters.at("mirror_id"), "registry-primary");

  const std::optional<spio::platform::RouteMatch> register_cluster =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Post, "/workgroups/local-dev/clusters/register");
  ASSERT_TRUE(register_cluster.has_value());
  EXPECT_EQ(register_cluster->route.operation_id, "registerWorkgroupCluster");
  EXPECT_EQ(register_cluster->parameters.at("workgroup_id"), "local-dev");

  const std::optional<spio::platform::RouteMatch> list_clusters =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Get, "/workgroups/local-dev/clusters");
  ASSERT_TRUE(list_clusters.has_value());
  EXPECT_EQ(list_clusters->route.operation_id, "listWorkgroupClusters");
  EXPECT_TRUE(list_clusters->route.internal);

  const std::optional<spio::platform::RouteMatch> register_container =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Post, "/compile-containers/register");
  ASSERT_TRUE(register_container.has_value());
  EXPECT_EQ(register_container->route.operation_id, "registerCompileContainer");
  EXPECT_TRUE(register_container->route.internal);

  const std::optional<spio::platform::RouteMatch> switch_container =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Post, "/compile-containers/container-01/switch-workspace");
  ASSERT_TRUE(switch_container.has_value());
  EXPECT_EQ(switch_container->route.operation_id, "switchCompileContainerWorkspace");
  EXPECT_EQ(switch_container->parameters.at("container_id"), "container-01");

  EXPECT_FALSE(spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Post, "/jobs/job-abc/events").has_value());
}

TEST(PlatformServiceRouterTests, MatchesRegistryControlPlaneRoutesWithContractBasePath)
{
  const std::vector<spio::platform::RouteSpec> routes = spio::platform::BuildRegistryControlPlaneRoutes();

  const std::optional<spio::platform::RouteMatch> status =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Get, "/api/spio-registry-control/v1/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->route.operation_id, "registryStatus");
  EXPECT_TRUE(status->route.internal);

  const std::optional<spio::platform::RouteMatch> descriptor =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Get, "/api/spio-registry-control/v1/descriptor");
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->route.operation_id, "registryDescriptor");
  EXPECT_TRUE(descriptor->route.internal);

  const std::optional<spio::platform::RouteMatch> publish =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Post, "/api/spio-registry-control/v1/publish");
  ASSERT_TRUE(publish.has_value());
  EXPECT_EQ(publish->route.operation_id, "publishRelease");

  const std::optional<spio::platform::RouteMatch> verify =
      spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Post, "/api/spio-registry-control/v1/verify");
  ASSERT_TRUE(verify.has_value());
  EXPECT_EQ(verify->route.operation_id, "verifyRegistry");

  const std::optional<spio::platform::RouteMatch> release =
      spio::platform::MatchRoute(
          routes,
          spio::platform::HttpMethod::Get,
          "/api/spio-registry-control/v1/packages/demo/app/releases/0.1.0");
  ASSERT_TRUE(release.has_value());
  EXPECT_EQ(release->route.operation_id, "getPackageRelease");
  EXPECT_EQ(release->parameters.at("namespace"), "demo");
  EXPECT_EQ(release->parameters.at("name"), "app");
  EXPECT_EQ(release->parameters.at("version"), "0.1.0");

  const std::optional<spio::platform::RouteMatch> remove_owner =
      spio::platform::MatchRoute(
          routes,
          spio::platform::HttpMethod::Delete,
          "/api/spio-registry-control/v1/packages/demo/app/owners/user-bob");
  ASSERT_TRUE(remove_owner.has_value());
  EXPECT_EQ(remove_owner->route.operation_id, "removePackageOwner");
}

TEST(PlatformPersistenceObjectStoreTests, SanitizesArtifactObjectKeyParts)
{
  const std::string key = spio::platform::BuildArtifactObjectKey(
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
  EXPECT_EQ(spio::platform::NormalizeObjectKey("/index/demo/app.jsonl"), "index/demo/app.jsonl");
  EXPECT_THROW(spio::platform::NormalizeObjectKey(""), std::runtime_error);
  EXPECT_THROW(spio::platform::NormalizeObjectKey("index/../root.json"), std::runtime_error);
  EXPECT_THROW(spio::platform::NormalizeObjectKey("index//root.json"), std::runtime_error);
  EXPECT_THROW(spio::platform::NormalizeObjectKey("index\\root.json"), std::runtime_error);
}

TEST(PlatformPersistencePostgresTests, DefinesCloudKernelMigrationAndClaimSql)
{
  const std::vector<spio::platform::SqlMigration> migrations = spio::platform::CloudKernelMigrations();

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

  const std::string claim_sql = spio::platform::ClaimJobSql();
  EXPECT_NE(claim_sql.find("FOR UPDATE SKIP LOCKED"), std::string::npos);
  EXPECT_NE(claim_sql.find("worker_pool_key = $2"), std::string::npos);
  EXPECT_NE(claim_sql.find("RETURNING *"), std::string::npos);

  const std::string complete_sql = spio::platform::CompleteJobSql();
  EXPECT_NE(complete_sql.find("status = 'running'"), std::string::npos);
  EXPECT_NE(complete_sql.find("worker_id = $4"), std::string::npos);
}

TEST(PlatformPersistenceRegistryTests, SerializesPackageRegistryDomainRecords)
{
  const spio::platform::RegistryPackageRecord package{
      .package_id = "demo/app",
      .package_namespace = "demo",
      .name = "app",
      .created_at = "2026-05-09T00:00:00Z",
      .created_by = "user-alice",
      .visibility = "public",
  };
  EXPECT_EQ(spio::platform::SerializeRegistryPackageRecord(package).at("namespace").get<std::string>(), "demo");

  const spio::platform::RegistryPackageReleaseRecord release{
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
  EXPECT_TRUE(spio::platform::SerializeRegistryPackageReleaseRecord(release).at("yanked").get<bool>());

  const spio::platform::RegistryPublishTokenRecord token{
      .token_id = "tok-000000000001",
      .token_hash = std::string(64, 'a'),
      .owner_id = "user-alice",
      .scopes = {"package:publish"},
      .package_patterns = {"demo/*"},
      .expires_at = "2026-06-09T00:00:00Z",
      .revoked_at = "",
      .created_at = "2026-05-09T00:00:00Z",
  };
  EXPECT_FALSE(spio::platform::SerializeRegistryPublishTokenRecord(token).contains("token_hash"));
  EXPECT_TRUE(spio::platform::SerializeRegistryPublishTokenRecord(token, true).contains("token_hash"));

  const spio::platform::RegistryPublicationRecord publication{
      .publication_id = "pub-000001",
      .repository_version_id = "rv-000001",
      .layout_version = 2,
      .root_path = "/registry/_publications/pub-000001",
      .manifest_sha256 = std::string(64, 'b'),
      .tree_size = 1,
      .created_at = "2026-05-09T00:00:00Z",
      .verified = true,
  };
  EXPECT_EQ(spio::platform::SerializeRegistryPublicationRecord(publication).at("layout_version").get<int>(), 2);

  const spio::platform::RegistryAuditEventRecord audit{
      .event_id = "audit-000000000001",
      .actor_id = "user-alice",
      .operation = "publishRelease",
      .target = {{"package_id", "demo/app"}},
      .request_id = "request-01",
      .result = "success",
      .created_at = "2026-05-09T00:00:00Z",
  };
  EXPECT_EQ(spio::platform::SerializeRegistryAuditEventRecord(audit).at("operation").get<std::string>(), "publishRelease");
}

TEST(PlatformPersistenceFactoryTests, BuildsCompileContainerRecordFromRegistrationPayload)
{
  const spio::platform::CompileContainerRecordFactory factory;
  const spio::platform::CompileContainerRecord record = factory.CreateFromRegistration({
      {"container_id", "container-01"},
      {"worker_id", "worker-01"},
      {"tenant_id", "tenant-acme"},
      {"user_id", "user-alice"},
      {"workspace_id", "workspace-main"},
      {"region", "local-dev"},
      {"worker_pool_key", "linux/x86_64/build/nightly/minimal"},
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
  spio::platform::PlatformConfig platform;
  platform.region = "local-dev";

  spio::platform::WorkerRuntimeConfig worker;
  worker.worker_id = "worker-01";
  worker.worker_pool_key = "linux/x86_64/build/nightly/minimal";
  worker.compile_container_id = "container-01";
  worker.compile_container_tenant_id = "tenant-acme";
  worker.compile_container_user_id = "user-alice";
  worker.compile_container_workspace_id = "workspace-main";
  worker.compile_container_capacity = 2;

  const spio::platform::WorkerCompileContainerFactory factory;
  const spio::platform::WorkerCompileContainerSpec spec = factory.Create(worker, platform);

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

TEST(PlatformCloudWorkerFactoryTests, CreatesWorkspaceUnderContainerRoot)
{
  const fs::path root = MakeTempDir("platform-worker-workspace-factory");

  spio::platform::WorkerRuntimeConfig worker;
  worker.workspace_root = root / "workspaces";
  worker.artifact_root = root / "artifacts";
  worker.compile_container_id = "container-01";

  const json job = {
      {"job_id", "job-0001"},
      {"tenant_id", "tenant-acme"},
      {"workspace_id", "workspace-main"},
      {"job_request", {{"manifest_path", "spio.toml"}}},
  };

  const spio::platform::WorkerWorkspaceFactory factory(spio::platform::DefaultOperatingSystemAdapter());
  const spio::platform::WorkerWorkspace workspace = factory.Create(worker, job);

  EXPECT_EQ(workspace.manifest_path, fs::path("spio.toml"));
  EXPECT_EQ(workspace.checkout_root, worker.workspace_root / "containers" / "container-01" / "source");
  EXPECT_EQ(workspace.stdout_path.filename(), "stdout.log");
  EXPECT_TRUE(fs::exists(workspace.checkout_root.parent_path()));
  EXPECT_TRUE(fs::exists(workspace.artifact_root));

  json absolute_manifest = job;
  absolute_manifest["job_request"]["manifest_path"] = "/tmp/spio.toml";
  EXPECT_THROW(factory.Create(worker, absolute_manifest), std::runtime_error);
}

TEST(PlatformServiceJobQueueTests, SubmitClaimCompleteLifecycleUsesSuccessEnvelopes)
{
  spio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.postgres_dsn = "postgres://platform@localhost/styio";
  config.object_store.provider = "memory";
  config.mtls.required = true;

  spio::platform::PlatformRouter router(config);

  const spio::platform::HttpResponse submit =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));
  ASSERT_EQ(submit.status_code, 200);
  ASSERT_EQ(submit.body.at("returncode").get<int>(), 0);
  EXPECT_EQ(submit.body.at("message").get<std::string>(), "queued platform job");
  const json queued = submit.body.at("payload");
  const std::string job_id = queued.at("job_id").get<std::string>();
  EXPECT_EQ(queued.at("status").get<std::string>(), "queued");
  EXPECT_EQ(queued.at("user_id").get<std::string>(), "user-alice");
  EXPECT_EQ(queued.at("worker_pool_key").get<std::string>(), "linux/x86_64/build/nightly/minimal");

  const spio::platform::HttpResponse register_worker =
      router.Dispatch(Request(
          spio::platform::HttpMethod::Post,
          "/workers/register",
          {
              {"worker_id", "worker-01"},
              {"region", "local-dev"},
              {"worker_pool_key", "linux/x86_64/build/nightly/minimal"},
              {"capacity", 1},
          }));
  ASSERT_EQ(register_worker.status_code, 200);
  EXPECT_EQ(register_worker.body.at("payload").at("status").get<std::string>(), "registered");

  const spio::platform::HttpResponse claim =
      router.Dispatch(Request(
          spio::platform::HttpMethod::Post,
          "/jobs/claim",
          {
              {"worker_id", "worker-01"},
              {"region", "local-dev"},
              {"worker_pool_key", "linux/x86_64/build/nightly/minimal"},
          }));
  ASSERT_EQ(claim.status_code, 200);
  ASSERT_TRUE(claim.body.at("payload").at("claimed").get<bool>());
  EXPECT_EQ(claim.body.at("payload").at("job").at("job_id").get<std::string>(), job_id);
  EXPECT_EQ(claim.body.at("payload").at("job").at("status").get<std::string>(), "running");
  EXPECT_EQ(claim.body.at("payload").at("job").at("worker_id").get<std::string>(), "worker-01");
  EXPECT_EQ(claim.body.at("payload").at("job").at("job_request").at("manifest_path").get<std::string>(), "spio.toml");

  const std::string artifact_key = spio::platform::BuildArtifactObjectKey("tenant-acme", "workspace-main", job_id, "stdout.log");
  const spio::platform::HttpResponse complete =
      router.Dispatch(Request(
          spio::platform::HttpMethod::Post,
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

  const spio::platform::HttpResponse events =
      router.Dispatch(Request(spio::platform::HttpMethod::Get, "/jobs/" + job_id + "/events"));
  ASSERT_EQ(events.status_code, 200);
  const json event_list = events.body.at("payload").at("events");
  ASSERT_EQ(event_list.size(), 3U);
  EXPECT_EQ(event_list.at(0).at("status").get<std::string>(), "queued");
  EXPECT_EQ(event_list.at(1).at("status").get<std::string>(), "running");
  EXPECT_EQ(event_list.at(2).at("status").get<std::string>(), "succeeded");
}

TEST(PlatformServiceJobQueueTests, CompileContainerHotSwitchesWorkspaceWithinUserBinding)
{
  spio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.object_store.provider = "memory";
  config.mtls.required = true;

  spio::platform::PlatformRouter router(config);

  const spio::platform::HttpResponse register_worker =
      router.Dispatch(Request(
          spio::platform::HttpMethod::Post,
          "/workers/register",
          {
              {"worker_id", "worker-01"},
              {"region", "local-dev"},
              {"worker_pool_key", "linux/x86_64/build/nightly/minimal"},
              {"capacity", 1},
          }));
  ASSERT_EQ(register_worker.status_code, 200);

  const spio::platform::HttpResponse register_container =
      router.Dispatch(Request(
          spio::platform::HttpMethod::Post,
          "/compile-containers/register",
          {
              {"container_id", "container-01"},
              {"worker_id", "worker-01"},
              {"tenant_id", "tenant-acme"},
              {"user_id", "user-alice"},
              {"workspace_id", "workspace-main"},
              {"region", "local-dev"},
              {"worker_pool_key", "linux/x86_64/build/nightly/minimal"},
              {"capacity", 1},
          }));
  ASSERT_EQ(register_container.status_code, 200);
  EXPECT_EQ(register_container.body.at("payload").at("user_id").get<std::string>(), "user-alice");
  EXPECT_EQ(register_container.body.at("payload").at("current_workspace_id").get<std::string>(), "workspace-main");

  const spio::platform::HttpResponse first_submit =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));
  ASSERT_EQ(first_submit.status_code, 200);
  const std::string first_job_id = first_submit.body.at("payload").at("job_id").get<std::string>();

  json other_user_request = MinimalJobRequest();
  other_user_request["user_id"] = "user-bob";
  other_user_request["workspace_id"] = "workspace-other";
  const spio::platform::HttpResponse other_submit =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs", other_user_request));
  ASSERT_EQ(other_submit.status_code, 200);

  json second_workspace_request = MinimalJobRequest();
  second_workspace_request["workspace_id"] = "workspace-feature";
  const spio::platform::HttpResponse second_submit =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs", second_workspace_request));
  ASSERT_EQ(second_submit.status_code, 200);
  const std::string second_job_id = second_submit.body.at("payload").at("job_id").get<std::string>();

  const json claim_body = {
      {"worker_id", "worker-01"},
      {"region", "local-dev"},
      {"worker_pool_key", "linux/x86_64/build/nightly/minimal"},
      {"compile_container_id", "container-01"},
  };

  const spio::platform::HttpResponse first_claim =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs/claim", claim_body));
  ASSERT_EQ(first_claim.status_code, 200);
  ASSERT_TRUE(first_claim.body.at("payload").at("claimed").get<bool>());
  EXPECT_EQ(first_claim.body.at("payload").at("job").at("job_id").get<std::string>(), first_job_id);
  EXPECT_EQ(first_claim.body.at("payload").at("compile_container").at("workspace_generation").get<int>(), 1);

  const spio::platform::HttpResponse first_complete =
      router.Dispatch(Request(
          spio::platform::HttpMethod::Post,
          "/jobs/" + first_job_id + "/complete",
          {
              {"worker_id", "worker-01"},
              {"status", "succeeded"},
              {"message", "build completed"},
          }));
  ASSERT_EQ(first_complete.status_code, 200);

  const spio::platform::HttpResponse second_claim =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs/claim", claim_body));
  ASSERT_EQ(second_claim.status_code, 200);
  ASSERT_TRUE(second_claim.body.at("payload").at("claimed").get<bool>());
  EXPECT_EQ(second_claim.body.at("payload").at("job").at("job_id").get<std::string>(), second_job_id);
  EXPECT_EQ(second_claim.body.at("payload").at("compile_container").at("user_id").get<std::string>(), "user-alice");
  EXPECT_EQ(second_claim.body.at("payload").at("compile_container").at("current_workspace_id").get<std::string>(), "workspace-feature");
  EXPECT_EQ(second_claim.body.at("payload").at("compile_container").at("workspace_generation").get<int>(), 2);

  const spio::platform::HttpResponse switched_events =
      router.Dispatch(Request(spio::platform::HttpMethod::Get, "/jobs/" + second_job_id + "/events"));
  ASSERT_EQ(switched_events.status_code, 200);
  ASSERT_GE(switched_events.body.at("payload").at("events").size(), 2U);
  EXPECT_EQ(switched_events.body.at("payload").at("events").at(0).at("message").get<std::string>(), "job queued");
  EXPECT_EQ(switched_events.body.at("payload").at("events").at(1).at("message").get<std::string>(), "compile container switched workspace");

  const spio::platform::HttpResponse wrong_user_switch =
      router.Dispatch(Request(
          spio::platform::HttpMethod::Post,
          "/compile-containers/container-01/switch-workspace",
          {
              {"worker_id", "worker-01"},
              {"tenant_id", "tenant-acme"},
              {"user_id", "user-bob"},
              {"workspace_id", "workspace-other"},
          }));
  EXPECT_EQ(wrong_user_switch.status_code, 403);

  const spio::platform::HttpResponse no_more_matching_work =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs/claim", claim_body));
  ASSERT_EQ(no_more_matching_work.status_code, 200);
  EXPECT_FALSE(no_more_matching_work.body.at("payload").at("claimed").get<bool>());
}

TEST(PlatformServiceJobQueueTests, RepeatedSubmissionsUseMonotonicIdsAndMissingMutationsDoNotCreateJobs)
{
  spio::platform::PlatformConfig config;
  config.region = "local-dev";
  config.node_id = "node-test";
  config.object_store.provider = "memory";
  config.mtls.required = true;

  spio::platform::PlatformRouter router(config);

  const spio::platform::HttpResponse first =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));
  const spio::platform::HttpResponse second =
      router.Dispatch(Request(spio::platform::HttpMethod::Post, "/jobs", MinimalJobRequest()));

  ASSERT_EQ(first.status_code, 200);
  ASSERT_EQ(second.status_code, 200);
  EXPECT_EQ(first.body.at("payload").at("job_id").get<std::string>(), "job-000000000001");
  EXPECT_EQ(second.body.at("payload").at("job_id").get<std::string>(), "job-000000000002");

  const spio::platform::HttpResponse cancel_missing = router.Dispatch(Request(
      spio::platform::HttpMethod::Post,
      "/jobs/job-000000999999/cancel",
      {{"reason", "missing"}}));
  EXPECT_EQ(cancel_missing.status_code, 404);

  const spio::platform::HttpResponse heartbeat_missing = router.Dispatch(Request(
      spio::platform::HttpMethod::Post,
      "/jobs/job-000000999999/heartbeat",
      {{"worker_id", "worker-01"}}));
  EXPECT_EQ(heartbeat_missing.status_code, 404);

  const spio::platform::HttpResponse complete_missing = router.Dispatch(Request(
      spio::platform::HttpMethod::Post,
      "/jobs/job-000000999999/complete",
      {{"worker_id", "worker-01"}, {"status", "succeeded"}}));
  EXPECT_EQ(complete_missing.status_code, 404);

  const spio::platform::HttpResponse lookup_missing =
      router.Dispatch(Request(spio::platform::HttpMethod::Get, "/jobs/job-000000999999"));
  EXPECT_EQ(lookup_missing.status_code, 404);
}

TEST(PlatformServiceWorkgroupTests, RegistersAndListsClustersWithDefaultPolicy)
{
  const fs::path root = MakeTempDir("platform-workgroup-register");
  spio::platform::PlatformConfig config = TestPlatformConfig(root);
  config.workgroup.registration_token = "local-token";

  spio::platform::PlatformRouter router(config);
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

  const spio::platform::HttpResponse registered = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
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

  const spio::platform::HttpResponse listed = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
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
  spio::platform::PlatformConfig config = TestPlatformConfig(root);
  config.workgroup.registration_token = "local-token";

  spio::platform::PlatformRouter router(config);
  const json registration = {
      {"cluster_id", "dev-a"},
      {"region", "local-dev"},
      {"node_id", "primary-0"},
      {"control_plane_endpoint", "http://127.0.0.1:8787/api/styio-platform/v1"},
      {"registration_token", "wrong-token"},
  };

  const spio::platform::HttpResponse worker_denied = router.Dispatch(Request(
      spio::platform::HttpMethod::Post,
      "/workgroups/local-dev/clusters/register",
      registration));
  ASSERT_EQ(worker_denied.status_code, 403);
  EXPECT_EQ(worker_denied.body.at("error_payload").at("category").get<std::string>(), "AuthError");

  const spio::platform::HttpResponse token_denied = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/workgroups/local-dev/clusters/register",
      OperatorIdentity(),
      registration));
  ASSERT_EQ(token_denied.status_code, 403);
  EXPECT_EQ(token_denied.body.at("error_payload").at("detail").get<std::string>(), "registration token is invalid");
}

TEST(PlatformRegistryControlPlaneTests, StatusUsesRedactedPathsAndFilesystemReadiness)
{
  const fs::path root = MakeTempDir("platform-registry-status");
  const spio::platform::PlatformConfig config = TestPlatformConfig(root);
  WriteFile(fs::path(config.registry.root) / "config.json", "{}\n");
  WriteFile(fs::path(config.registry.root) / "trust/root.json", "{}\n");
  fs::create_directories(config.registry.key_dir);

  spio::platform::PlatformRouter router(config);
  const spio::platform::HttpResponse status = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/status",
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
  EXPECT_EQ(payload.at("publish_endpoint").get<std::string>(), "/api/spio-registry-control/v1/publish");
  EXPECT_EQ(payload.at("verify_endpoint").get<std::string>(), "/api/spio-registry-control/v1/verify");
  EXPECT_EQ(payload.at("descriptor_endpoint").get<std::string>(), "/api/spio-registry-control/v1/descriptor");
  EXPECT_EQ(status.body.dump().find(config.registry.root), std::string::npos);
  EXPECT_EQ(status.body.dump().find(config.registry.key_dir), std::string::npos);
}

TEST(PlatformRegistryControlPlaneTests, PublishVerifyAndMirrorStatusUseLocalState)
{
  const fs::path root = MakeTempDir("platform-registry-publish");
  const spio::platform::PlatformConfig config = TestPlatformConfig(root);
  WriteFile(
      root / "workspace/spio.toml",
      "[spio]\n"
      "manifest-version = 1\n\n"
      "[package]\n"
      "name = \"demo/app\"\n"
      "version = \"0.1.0\"\n"
      "edition = \"2026\"\n"
      "publish = true\n\n"
      "[toolchain]\n"
      "channel = \"nightly\"\n"
      "implicit-std = true\n\n"
      "[lib]\n"
      "path = \"src/lib.styio\"\n");

  spio::platform::PlatformRouter router(config);
  const spio::platform::HttpResponse publish = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      {
          {"manifest_path", (root / "workspace/spio.toml").string()},
          {"publisher_id", "registry-writer-01"},
      }));

  ASSERT_EQ(publish.status_code, 200);
  ASSERT_EQ(publish.body.at("returncode").get<int>(), 0);
  const json published = publish.body.at("payload");
  EXPECT_EQ(published.at("package").get<std::string>(), "demo/app");
  EXPECT_EQ(published.at("version").get<std::string>(), "0.1.0");
  EXPECT_TRUE(published.at("created_root").get<bool>());
  EXPECT_EQ(published.at("sequence").get<int>(), 1);
  EXPECT_EQ(published.at("archive_sha256").get<std::string>().size(), 64U);
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / published.at("artifact_path").get<std::string>()));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / published.at("index_path").get<std::string>()));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / published.at("log_leaf_path").get<std::string>()));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "trust/timestamp.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "trust/snapshot.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "trust/targets/demo.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "log/checkpoint.json"));

  const json registry_config = json::parse(ReadFile(fs::path(config.registry.root) / "config.json"));
  EXPECT_EQ(registry_config.at("protocol").get<std::string>(), "spio-static-registry");
  EXPECT_EQ(registry_config.at("protocol_version").get<int>(), 2);
  const json root_metadata = json::parse(ReadFile(fs::path(config.registry.root) / "trust/root.json"));
  EXPECT_EQ(root_metadata.at("signed").at("type").get<std::string>(), "root");
  ASSERT_FALSE(root_metadata.at("signatures").empty());

  const spio::platform::HttpResponse descriptor = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/descriptor",
      RegistryWriterIdentity()));
  ASSERT_EQ(descriptor.status_code, 200);
  ASSERT_EQ(descriptor.body.at("returncode").get<int>(), 0);
  const json descriptor_payload = descriptor.body.at("payload");
  EXPECT_EQ(descriptor_payload.at("schema_version").get<int>(), 1);
  EXPECT_EQ(descriptor_payload.at("registry_name").get<std::string>(), "test-registry");
  EXPECT_EQ(descriptor_payload.at("root_sha256").get<std::string>().size(), 64U);
  EXPECT_EQ(descriptor_payload.at("control_plane_base_url").get<std::string>(), "/api/spio-registry-control/v1");
  EXPECT_EQ(descriptor_payload.at("descriptor_signature").get<std::string>(), "platform-control-plane-mtls");

  const spio::platform::HttpResponse verify = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/verify",
      MirrorIdentity(),
      json::object()));
  ASSERT_EQ(verify.status_code, 200);
  EXPECT_TRUE(verify.body.at("payload").at("ok").get<bool>());
  EXPECT_EQ(verify.body.at("payload").at("namespaces").get<int>(), 1);
  EXPECT_EQ(verify.body.at("payload").at("index_files").get<int>(), 1);
  EXPECT_EQ(verify.body.at("payload").at("releases").get<int>(), 1);
  EXPECT_EQ(verify.body.at("payload").at("tree_size").get<int>(), 1);

  const spio::platform::HttpResponse mirror = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
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
  const spio::platform::PlatformConfig config = TestPlatformConfig(root);
  WriteFile(
      root / "workspace/spio.toml",
      "[spio]\n"
      "manifest-version = 1\n\n"
      "[package]\n"
      "name = \"demo/app\"\n"
      "version = \"0.1.0\"\n"
      "edition = \"2026\"\n"
      "publish = true\n\n"
      "[toolchain]\n"
      "channel = \"nightly\"\n"
      "implicit-std = true\n\n"
      "[lib]\n"
      "path = \"src/lib.styio\"\n");

  spio::platform::PlatformRouter router(config);
  const json publish_request = {
      {"manifest_path", (root / "workspace/spio.toml").string()},
      {"publisher_id", "registry-writer-01"},
  };
  const spio::platform::HttpResponse publish = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      publish_request));
  ASSERT_EQ(publish.status_code, 200);
  EXPECT_EQ(publish.body.at("payload").at("publication_id").get<std::string>(), "pub-000001");
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "_publications/pub-000001/publication.json"));
  EXPECT_TRUE(fs::exists(fs::path(config.registry.root) / "_distributions/default/current.json"));

  const spio::platform::HttpResponse package = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/packages/demo/app",
      RegistryWriterIdentity()));
  ASSERT_EQ(package.status_code, 200);
  EXPECT_EQ(package.body.at("payload").at("latest_version").get<std::string>(), "0.1.0");

  const spio::platform::HttpResponse release = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/packages/demo/app/releases/0.1.0",
      RegistryWriterIdentity()));
  ASSERT_EQ(release.status_code, 200);
  EXPECT_FALSE(release.body.at("payload").at("yanked").get<bool>());

  const spio::platform::HttpResponse add_owner = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/packages/demo/app/owners",
      RegistryWriterIdentity(),
      {{"owner_id", "user-bob"}, {"owner_kind", "user"}, {"role", "owner"}}));
  ASSERT_EQ(add_owner.status_code, 200);
  const spio::platform::HttpResponse owners = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/packages/demo/app/owners",
      RegistryWriterIdentity()));
  ASSERT_EQ(owners.status_code, 200);
  EXPECT_EQ(owners.body.at("payload").at("owners").size(), 2U);
  const spio::platform::HttpResponse remove_owner = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Delete,
      "/api/spio-registry-control/v1/packages/demo/app/owners/user-bob",
      RegistryWriterIdentity()));
  ASSERT_EQ(remove_owner.status_code, 200);

  const spio::platform::HttpResponse token_created = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/tokens",
      RegistryWriterIdentity(),
      {
          {"scopes", json::array({"package:publish", "package:yank", "package:owner", "repository:promote"})},
          {"package_patterns", json::array({"demo/*"})},
      }));
  ASSERT_EQ(token_created.status_code, 200);
  const std::string token = token_created.body.at("payload").at("token").get<std::string>();
  EXPECT_FALSE(token_created.body.at("payload").contains("token_hash"));

  const spio::platform::HttpResponse denied_publish = router.Dispatch(RequestWithToken(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/publish",
      token,
      {{"package", "other/app"}, {"version", "1.0.0"}, {"publisher_id", "token-user"}}));
  ASSERT_EQ(denied_publish.status_code, 403);

  const spio::platform::HttpResponse yank = router.Dispatch(RequestWithToken(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/packages/demo/app/releases/0.1.0/yank",
      token,
      {{"reason", "bad metadata"}}));
  ASSERT_EQ(yank.status_code, 200);
  EXPECT_TRUE(yank.body.at("payload").at("yanked").get<bool>());
  EXPECT_TRUE(yank.body.at("payload").at("artifact_preserved").get<bool>());
  EXPECT_EQ(yank.body.at("payload").at("publication_id").get<std::string>(), "pub-000002");

  const spio::platform::HttpResponse yanked_release = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/packages/demo/app/releases/0.1.0",
      RegistryWriterIdentity()));
  ASSERT_EQ(yanked_release.status_code, 200);
  EXPECT_TRUE(yanked_release.body.at("payload").at("yanked").get<bool>());

  const spio::platform::HttpResponse versions = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/repositories/default/versions",
      RegistryWriterIdentity()));
  ASSERT_EQ(versions.status_code, 200);
  EXPECT_EQ(versions.body.at("payload").at("versions").size(), 2U);

  const spio::platform::HttpResponse publication = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/publications/pub-000002",
      RegistryWriterIdentity()));
  ASSERT_EQ(publication.status_code, 200);
  EXPECT_TRUE(publication.body.at("payload").at("verified").get<bool>());

  const spio::platform::HttpResponse rollback = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/distributions/default/rollback",
      OperatorIdentity(),
      json::object()));
  ASSERT_EQ(rollback.status_code, 200);
  EXPECT_EQ(rollback.body.at("payload").at("publication_id").get<std::string>(), "pub-000001");

  const spio::platform::HttpResponse promote = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/distributions/default/promote",
      OperatorIdentity(),
      {{"publication_id", "pub-000002"}}));
  ASSERT_EQ(promote.status_code, 200);
  EXPECT_EQ(promote.body.at("payload").at("publication_id").get<std::string>(), "pub-000002");

  const spio::platform::HttpResponse unyank = router.Dispatch(RequestWithToken(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/packages/demo/app/releases/0.1.0/unyank",
      token,
      json::object()));
  ASSERT_EQ(unyank.status_code, 200);
  EXPECT_FALSE(unyank.body.at("payload").at("yanked").get<bool>());
  EXPECT_TRUE(unyank.body.at("payload").at("artifact_preserved").get<bool>());
  EXPECT_EQ(unyank.body.at("payload").at("publication_id").get<std::string>(), "pub-000003");

  const spio::platform::HttpResponse final_release = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Get,
      "/api/spio-registry-control/v1/packages/demo/app/releases/0.1.0",
      RegistryWriterIdentity()));
  ASSERT_EQ(final_release.status_code, 200);
  EXPECT_FALSE(final_release.body.at("payload").at("yanked").get<bool>());

  const spio::platform::HttpResponse revoked = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Delete,
      "/api/spio-registry-control/v1/tokens/" + token_created.body.at("payload").at("token_id").get<std::string>(),
      RegistryWriterIdentity()));
  ASSERT_EQ(revoked.status_code, 200);

  const spio::platform::HttpResponse revoked_token_denied = router.Dispatch(RequestWithToken(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/packages/demo/app/releases/0.1.0/yank",
      token,
      json::object()));
  ASSERT_EQ(revoked_token_denied.status_code, 403);

  spio::platform::PlatformConfig mirror_config = config;
  mirror_config.registry.root = (root / "mirror-registry").string();
  mirror_config.registry.mirror_source_root = config.registry.root;
  ASSERT_EQ(spio::platform::RunMirrorSyncOnce(mirror_config), 0);
  const json mirror_current = json::parse(ReadFile(fs::path(mirror_config.registry.root) / "_distributions/default/current.json"));
  EXPECT_EQ(mirror_current.at("publication_id").get<std::string>(), "pub-000003");
  EXPECT_TRUE(fs::exists(fs::path(mirror_config.registry.root) / "_publications/pub-000003/publication.json"));
}

TEST(PlatformRegistryControlPlaneTests, EnforcesRegistryRolesAndNonSuccessDomainErrors)
{
  const fs::path root = MakeTempDir("platform-registry-errors");
  const spio::platform::PlatformConfig config = TestPlatformConfig(root);
  WriteFile(
      root / "workspace/spio.toml",
      "[spio]\n"
      "manifest-version = 1\n\n"
      "[package]\n"
      "name = \"demo/app\"\n"
      "version = \"0.1.0\"\n"
      "edition = \"2026\"\n"
      "publish = true\n\n"
      "[toolchain]\n"
      "channel = \"nightly\"\n"
      "implicit-std = true\n\n"
      "[lib]\n"
      "path = \"src/lib.styio\"\n");

  spio::platform::PlatformRouter router(config);
  const json publish_request = {
      {"manifest_path", (root / "workspace/spio.toml").string()},
      {"publisher_id", "registry-writer-01"},
  };

  const spio::platform::HttpResponse denied = router.Dispatch(Request(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/publish",
      publish_request));
  ASSERT_EQ(denied.status_code, 403);
  EXPECT_EQ(denied.body.at("returncode").get<int>(), 2);

  const spio::platform::HttpResponse verify_before_publish = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/verify",
      MirrorIdentity(),
      json::object()));
  ASSERT_EQ(verify_before_publish.status_code, 422);
  EXPECT_EQ(verify_before_publish.body.at("error_payload").at("category").get<std::string>(), "VerifyError");

  const spio::platform::HttpResponse first_publish = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      publish_request));
  ASSERT_EQ(first_publish.status_code, 200);

  const spio::platform::HttpResponse duplicate = router.Dispatch(RequestWithIdentity(
      spio::platform::HttpMethod::Post,
      "/api/spio-registry-control/v1/publish",
      RegistryWriterIdentity(),
      publish_request));
  ASSERT_EQ(duplicate.status_code, 409);
  EXPECT_EQ(duplicate.body.at("returncode").get<int>(), 17);
  EXPECT_EQ(duplicate.body.at("error_payload").at("category").get<std::string>(), "PublishError");
  EXPECT_FALSE(duplicate.body.at("error_payload").contains("operation_id"));
}
