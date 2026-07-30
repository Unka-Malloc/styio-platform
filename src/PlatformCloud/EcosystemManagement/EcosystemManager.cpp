#include "PlatformCloud/EcosystemManagement/EcosystemManager.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace spio::platform
{

namespace
{

nlohmann::json Repository(
    std::string id,
    std::string origin,
    std::string runtime_kind,
    std::string adapter,
    std::vector<std::string> artifacts,
    std::vector<std::string> contracts,
    std::vector<std::string> gates)
{
  return {
      {"id", std::move(id)},
      {"branch", "stable"},
      {"origin", std::move(origin)},
      {"runtime", {{"kind", std::move(runtime_kind)}, {"adapter", std::move(adapter)}}},
      {"artifacts", std::move(artifacts)},
      {"contracts", std::move(contracts)},
      {"required_gates", std::move(gates)},
  };
}

std::vector<nlohmann::json> RepositoryList()
{
  return {
      Repository(
          "styio",
          "https://github.com/eBioRing/Styio.git",
          "native-toolchain",
          "cmake-native",
          {"styio-compiler", "styio-stdlib"},
          {"machine-info/v1", "compile-plan/v1", "diagnostics/v1", "receipt/v1", "runtime-events/v1"},
          {"build", "native-test", "compiler-smoke"}),
      Repository(
          "pafio-nightly",
          "https://github.com/Unka-Malloc/pafio-nightly.git",
          "native-cli",
          "cmake-native",
          {"pafio-cli", "package-manager-archive"},
          {"metadata/v1", "workflow/v1", "registry-v2/v1"},
          {"build", "native-test", "registry-interop"}),
      Repository(
          "vityo-nightly",
          "https://github.com/Unka-Malloc/vityo-nightly.git",
          "frontend",
          "flutter-app",
          {"vityo-app-bundle", "desktop-shell-bundle"},
          {"hosted-control-plane/v1", "platform-control-plane/v1"},
          {"install", "flutter-analyze", "flutter-test", "web-build"}),
      Repository(
          "styio-platform",
          "https://github.com/eBioRing/styio-platform.git",
          "server",
          "cmake-server",
          {"styio-platformd", "registry-server-bundle", "helm-chart"},
          {"platform-control-plane/v1", "registry-control-plane/v1", "registry-v2/v1"},
          {"build", "ctest", "docs-gate", "contract-gate"}),
      Repository(
          "styio-community",
          "https://github.com/eBioRing/styio-community.git",
          "docs-site",
          "static-docs",
          {"community-site", "examples-index"},
          {"community-governance/v1"},
          {"link-check", "docs-build", "examples-smoke"}),
  };
}

std::map<std::string, nlohmann::json> RepositoryMap()
{
  std::map<std::string, nlohmann::json> repos;
  for (const nlohmann::json &repo : RepositoryList())
  {
    repos.emplace(repo.at("id").get<std::string>(), repo);
  }
  return repos;
}

std::string RefForRepository(const nlohmann::json &request, const std::string &repo_id)
{
  if (request.contains("components") && request["components"].is_object())
  {
    const nlohmann::json &components = request["components"];
    if (components.contains(repo_id))
    {
      if (!components[repo_id].is_string() || components[repo_id].get<std::string>().empty())
      {
        throw std::invalid_argument("component ref must be a non-empty string: " + repo_id);
      }
      return components[repo_id].get<std::string>();
    }
  }
  if (request.contains("version") && request["version"].is_string() && !request["version"].get<std::string>().empty())
  {
    return request["version"].get<std::string>();
  }
  return "stable";
}

}  // namespace

nlohmann::json DefaultEcosystemRepositories()
{
  nlohmann::json repos = nlohmann::json::array();
  for (const nlohmann::json &repo : RepositoryList())
  {
    repos.push_back(repo);
  }
  return {
      {"ecosystem_id", "styio"},
      {"branch_policy", {{"single_branch", "stable"}, {"release_refs", "tags-or-stable-ref"}}},
      {"repositories", repos},
  };
}

nlohmann::json BuildEcosystemReleasePlan(const nlohmann::json &request)
{
  if (!request.is_object())
  {
    throw std::invalid_argument("request body must be an object");
  }
  if (request.contains("components") && !request["components"].is_object())
  {
    throw std::invalid_argument("components must be an object when present");
  }
  if (request.contains("components"))
  {
    const std::map<std::string, nlohmann::json> repos = RepositoryMap();
    for (const auto &[repo_id, ignored] : request["components"].items())
    {
      (void) ignored;
      if (!repos.contains(repo_id))
      {
        throw std::invalid_argument("unknown ecosystem repository: " + repo_id);
      }
    }
  }

  const std::string release_id =
      request.value("release_id", request.value("version", std::string("stable")));
  nlohmann::json components = nlohmann::json::array();
  nlohmann::json execution = nlohmann::json::array();
  for (const nlohmann::json &repo : RepositoryList())
  {
    const std::string repo_id = repo.at("id").get<std::string>();
    const std::string ref = RefForRepository(request, repo_id);
    components.push_back({
        {"repository_id", repo_id},
        {"origin", repo.at("origin")},
        {"branch", repo.at("branch")},
        {"ref", ref},
        {"adapter", repo.at("runtime").at("adapter")},
        {"artifacts", repo.at("artifacts")},
    });
    execution.push_back({
        {"step", static_cast<int>(execution.size()) + 1},
        {"repository_id", repo_id},
        {"adapter", repo.at("runtime").at("adapter")},
        {"fetch", {{"origin", repo.at("origin")}, {"ref", ref}}},
        {"gates", repo.at("required_gates")},
        {"publish_artifacts", repo.at("artifacts")},
    });
  }

  return {
      {"ecosystem_id", "styio"},
      {"release_id", release_id},
      {"branch", "stable"},
      {"components", components},
      {"execution_plan", execution},
      {"invariants",
       {"all repositories use stable as the only long-lived branch",
        "release refs are immutable tags or the stable ref for unreleased integration",
        "runtime adapters are selected per repository and never force one shared runtime"}},
  };
}

}  // namespace spio::platform
