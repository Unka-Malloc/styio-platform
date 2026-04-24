#include "BuildTestSupport.hpp"

#include "SpioCloud/Contract.hpp"
#include "SpioCloud/Execution.hpp"
#include "SpioCloud/Job.hpp"
#include "SpioCore/Errors.hpp"
#include "SpioPlan/CompilePlan.hpp"
#include "PlatformService/Http.hpp"
#include "PlatformService/Identity.hpp"
#include "PlatformService/ObjectStore.hpp"
#include "PlatformService/PostgresStore.hpp"
#include "PlatformService/Router.hpp"

#include <nlohmann/json.hpp>

#include <string>

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

nlohmann::json MinimalJobRequest()
{
  return {
      {"tenant_id", "tenant-acme"},
      {"workspace_id", "workspace-main"},
      {"action", "build"},
      {"region", "local-dev"},
      {"preferred_worker_pool", "linux/x86_64/build/nightly/minimal"},
      {"job_request", {
                          {"manifest_path", "spio.toml"},
                          {"profile", "dev"},
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

}  // namespace

TEST(PlatformServiceIdentityTests, ParsesMtlsUriSanIntoRoleTenantAndNode)
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

TEST(PlatformServiceIdentityTests, RejectsUnknownMtlsUriSanRoleOrMissingNode)
{
  EXPECT_FALSE(spio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/acme/role/browser/node/client").has_value());
  EXPECT_FALSE(spio::platform::ParseMtlsUriSan("spiffe://styio-platform/tenant/acme/role/worker").has_value());
  EXPECT_FALSE(spio::platform::ParseMtlsUriSan("https://styio-platform/tenant/acme/role/worker/node/worker-01").has_value());
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

  EXPECT_FALSE(spio::platform::MatchRoute(routes, spio::platform::HttpMethod::Post, "/jobs/job-abc/events").has_value());
}

TEST(PlatformServiceObjectStoreTests, SanitizesArtifactObjectKeyParts)
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

TEST(PlatformServicePostgresTests, DefinesCloudKernelMigrationAndClaimSql)
{
  const std::vector<spio::platform::SqlMigration> migrations = spio::platform::CloudKernelMigrations();

  ASSERT_FALSE(migrations.empty());
  EXPECT_EQ(migrations.front().id, "001_cloud_kernel");
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_jobs"), std::string::npos);
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_job_events"), std::string::npos);
  EXPECT_NE(migrations.front().sql.find("CREATE TABLE IF NOT EXISTS platform_artifacts"), std::string::npos);

  const std::string claim_sql = spio::platform::ClaimJobSql();
  EXPECT_NE(claim_sql.find("FOR UPDATE SKIP LOCKED"), std::string::npos);
  EXPECT_NE(claim_sql.find("worker_pool_key = $2"), std::string::npos);
  EXPECT_NE(claim_sql.find("RETURNING *"), std::string::npos);

  const std::string complete_sql = spio::platform::CompleteJobSql();
  EXPECT_NE(complete_sql.find("status = 'running'"), std::string::npos);
  EXPECT_NE(complete_sql.find("worker_id = $4"), std::string::npos);
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
