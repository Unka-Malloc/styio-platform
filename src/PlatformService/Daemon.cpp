#include "PlatformCore/Config.hpp"
#include "PlatformService/BeastServer.hpp"
#include "PlatformService/Http.hpp"
#include "PlatformSecurity/PlatformClientAuth/Identity.hpp"
#include "PlatformCloud/PackageRegistry/MirrorSync/MirrorSync.hpp"
#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"
#include "PlatformService/Router.hpp"
#include "PlatformCloud/DeveloperWorkspace/Worker.hpp"

#include <iostream>
#include <string>

namespace
{

void PrintUsage()
{
  std::cout
      << "Usage: styio-platformd [--check-config|--migrate|--serve|--serve-once|--worker|--worker-once|--sync-mirror-once|--self-test|--print-routes|--print-migrations]\n";
}

spio::platform::MtlsIdentity SelfTestIdentity()
{
  return {
      .role = "operator",
      .tenant_id = "tenant-demo",
      .node_id = "node-local",
  };
}

}  // namespace

int main(int argc, char **argv)
{
  const std::string command = argc > 1 ? argv[1] : "--check-config";
  spio::platform::PlatformConfig config = spio::platform::LoadPlatformConfigFromEnvironment();
  spio::platform::PlatformRouter router(config);

  if (command == "--help" || command == "-h")
  {
    PrintUsage();
    return 0;
  }
  if (command == "--check-config")
  {
    nlohmann::json payload = spio::platform::SerializePublicConfig(config);
    payload["http_adapter"] = spio::platform::DescribeBeastServerCapability();
    std::cout << payload.dump(2) << "\n";
    return 0;
  }
  if (command == "--serve")
  {
    return spio::platform::RunBeastServer(config);
  }
  if (command == "--serve-once")
  {
    return spio::platform::RunBeastServer(config, {.once = true});
  }
  if (command == "--migrate")
  {
    try
    {
      spio::platform::PostgresStore store(config.postgres_dsn);
      store.ApplyMigrations();
      store.UpsertNode(config);
      std::cout << "styio-platform postgres migrations applied\n";
      return 0;
    }
    catch (const std::exception &error)
    {
      std::cerr << "styio-platform migration failed: " << error.what() << "\n";
      return 1;
    }
  }
  if (command == "--worker")
  {
    return spio::platform::RunWorker(config);
  }
  if (command == "--worker-once")
  {
    return spio::platform::RunWorker(config, {.once = true});
  }
  if (command == "--sync-mirror-once")
  {
    return spio::platform::RunMirrorSyncOnce(config);
  }
  if (command == "--print-routes")
  {
    nlohmann::json routes = nlohmann::json::array();
    for (const spio::platform::RouteSpec &route : router.routes())
    {
      routes.push_back({
          {"operation_id", route.operation_id},
          {"method", spio::platform::ToString(route.method)},
          {"path", route.path},
          {"internal", route.internal},
      });
    }
    std::cout << routes.dump(2) << "\n";
    return 0;
  }
  if (command == "--print-migrations")
  {
    for (const spio::platform::SqlMigration &migration : spio::platform::CloudKernelMigrations())
    {
      std::cout << "-- " << migration.id << "\n" << migration.sql << "\n";
    }
    return 0;
  }
  if (command == "--self-test")
  {
    const spio::platform::HttpResponse health = router.Dispatch({
        .method = spio::platform::HttpMethod::Get,
        .path = "/health",
        .identity = SelfTestIdentity(),
    });
    const spio::platform::HttpResponse submit = router.Dispatch({
        .method = spio::platform::HttpMethod::Post,
        .path = "/jobs",
        .body = {
            {"tenant_id", "tenant-demo"},
            {"user_id", "user-demo"},
            {"workspace_id", "workspace-demo"},
            {"action", "build"},
            {"preferred_worker_pool", "linux/x86_64/build/nightly/minimal"},
            {"job_request",
             {
                 {"schema_version", 1},
                 {"api_path", "/api/styio-platform/v1/jobs"},
                 {"action", "build"},
                 {"manifest_path", "spio.toml"},
                 {"source", {{"origin", "file:///tmp/styio-platform-self-test"}}},
                 {"toolchain", nlohmann::json::object()},
                 {"workflow", nlohmann::json::object()},
                 {"target", nlohmann::json::object()},
                 {"cloud", nlohmann::json::object()},
             }},
        },
        .identity = SelfTestIdentity(),
    });
    std::cout << nlohmann::json{{"health", health.body}, {"submit", submit.body}}.dump(2) << "\n";
    return health.status_code == 200 && submit.status_code == 200 ? 0 : 1;
  }

  std::cerr << "unsupported command: " << command << "\n";
  PrintUsage();
  return 2;
}
