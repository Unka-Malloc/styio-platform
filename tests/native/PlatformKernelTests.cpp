#include "BuildTestSupport.hpp"

#include "SpioCloud/Contract.hpp"
#include "SpioCloud/Execution.hpp"
#include "SpioCloud/Job.hpp"
#include "SpioCore/Errors.hpp"
#include "SpioPlan/CompilePlan.hpp"

#include <nlohmann/json.hpp>

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

  EXPECT_EQ(payload.at("api_path").get<std::string>(), "/v1/build-jobs");
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
