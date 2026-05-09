#include "PlatformCloud/DocumentationGovernance/DocumentationGovernance.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace spio::platform
{

namespace
{

struct DocumentationCollection
{
  std::string id;
  std::string path;
  std::string owner;
  std::string classification;
  std::vector<std::string> purposes;
  std::vector<std::string> gates;
};

bool StartsWith(std::string_view text, std::string_view prefix)
{
  return text.substr(0, prefix.size()) == prefix;
}

nlohmann::json CollectionJson(const DocumentationCollection &collection)
{
  return {
      {"id", collection.id},
      {"path", collection.path},
      {"owner", collection.owner},
      {"classification", collection.classification},
      {"purposes", collection.purposes},
      {"required_gates", collection.gates},
  };
}

std::vector<DocumentationCollection> CollectionList()
{
  return {
      {"governance",
       "docs/governance",
       "control-plane",
       "normative",
       {"service-boundary policy", "control-plane rules", "workspace compile model"},
       {"docs-index", "docs-audit", "team-docs-gate", "contract-gate"}},
      {"registry",
       "docs/registry",
       "control-plane",
       "normative",
       {"package registry contracts", "mirror synchronization", "static read-plane policy"},
       {"docs-index", "docs-audit", "team-docs-gate", "registry-contract-gate"}},
      {"operations",
       "docs/operations",
       "control-plane",
       "operational",
       {"regional node operations", "registry server deployment", "compile stress operation"},
       {"docs-index", "docs-audit", "team-docs-gate", "smoke-gate"}},
      {"security",
       "docs/security",
       "platform-security",
       "normative",
       {"trust boundaries", "hosted workspace security", "registry security"},
       {"docs-index", "docs-audit", "security-review"}},
      {"specs",
       "docs/specs",
       "docs-delivery",
       "normative",
       {"technology inventory", "post-commit checks", "audit checklist"},
       {"docs-index", "docs-audit", "repo-hygiene-gate"}},
      {"adr",
       "docs/adr",
       "architecture",
       "decision-record",
       {"architecture decisions", "migration rationale"},
       {"docs-index", "docs-audit"}},
      {"planning",
       "docs/planning",
       "platform-kernel",
       "planning",
       {"migration plans", "roadmap items"},
       {"docs-index", "docs-audit"}},
      {"external",
       "docs/external",
       "docs-delivery",
       "handoff",
       {"downstream handoff", "cross-repository alignment"},
       {"docs-index", "docs-audit"}},
      {"assets",
       "docs/assets",
       "docs-delivery",
       "workflow-asset",
       {"reusable workflow assets", "gate descriptions", "templates"},
       {"docs-index", "docs-audit"}},
      {"teams",
       "docs/teams",
       "docs-delivery",
       "ownership-runbook",
       {"team ownership", "handoff policy", "runbook statistics"},
       {"team-docs-gate", "docs-audit"}},
      {"audit",
       "docs/audit",
       "docs-delivery",
       "audit-evidence",
       {"public audit summaries", "transient defect inventory boundary"},
       {"docs-index", "docs-audit", "audit-gate"}},
  };
}

auto DocsBoundary(
    std::string repository_id,
    std::string documentation_owner,
    std::vector<std::string> primary_topics) -> nlohmann::json
{
  return {
      {"repository_id", std::move(repository_id)},
      {"documentation_owner", std::move(documentation_owner)},
      {"primary_topics", std::move(primary_topics)},
  };
}

std::vector<nlohmann::json> EcosystemDocsBoundaries()
{
  return {
      DocsBoundary("styio", "language-and-compiler", {"language reference", "compiler behavior", "standard library"}),
      DocsBoundary("styio-spio", "package-manager", {"CLI usage", "package resolution", "registry client behavior"}),
      DocsBoundary("styio-view", "product-experience", {"hosted workspace UX", "control console flows", "frontend contract usage"}),
      DocsBoundary("styio-platform", "platform-foundation", {"control-plane contracts", "package registry", "cloud workspaces", "operations"}),
      DocsBoundary("styio-community", "community", {"tutorials", "examples", "public ecosystem guides"}),
  };
}

std::string CollectionForPath(const std::string &path)
{
  std::string best;
  std::size_t best_size = 0;
  for (const DocumentationCollection &collection : CollectionList())
  {
    const std::string prefix = collection.path + "/";
    if ((path == collection.path || StartsWith(path, prefix)) && collection.path.size() > best_size)
    {
      best = collection.id;
      best_size = collection.path.size();
    }
  }
  if (!best.empty())
  {
    return best;
  }
  if (StartsWith(path, "contracts/"))
  {
    return "contracts";
  }
  if (StartsWith(path, "manifests/"))
  {
    return "business-manifests";
  }
  if (StartsWith(path, "scripts/") && path.find("docs") != std::string::npos)
  {
    return "docs-automation";
  }
  return "unclassified";
}

std::vector<std::string> GatesForCollection(const std::string &collection_id)
{
  for (const DocumentationCollection &collection : CollectionList())
  {
    if (collection.id == collection_id)
    {
      return collection.gates;
    }
  }
  if (collection_id == "contracts")
  {
    return {"contract-gate", "docs-audit", "team-docs-gate"};
  }
  if (collection_id == "business-manifests")
  {
    return {"manifest-review", "docs-audit", "team-docs-gate"};
  }
  if (collection_id == "docs-automation")
  {
    return {"docs-index", "docs-audit", "team-docs-gate", "repo-hygiene-gate"};
  }
  return {"docs-audit", "repo-hygiene-gate"};
}

std::vector<std::string> RunbooksForPath(const std::string &path)
{
  std::set<std::string> runbooks;
  if (StartsWith(path, "docs/") || StartsWith(path, "scripts/docs") || path == "README.md")
  {
    runbooks.insert("docs/teams/DOCS-DELIVERY-RUNBOOK.md");
  }
  if (StartsWith(path, "contracts/") || StartsWith(path, "docs/governance/") ||
      StartsWith(path, "docs/registry/") || StartsWith(path, "docs/operations/") ||
      StartsWith(path, "tests/interop/"))
  {
    runbooks.insert("docs/teams/CONTROL-PLANE-RUNBOOK.md");
  }
  if (StartsWith(path, "src/") || StartsWith(path, "tests/native/") || StartsWith(path, "manifests/") ||
      path == "CMakeLists.txt")
  {
    runbooks.insert("docs/teams/PLATFORM-KERNEL-RUNBOOK.md");
  }
  return {runbooks.begin(), runbooks.end()};
}

void AddAll(std::set<std::string> &target, const std::vector<std::string> &values)
{
  target.insert(values.begin(), values.end());
}

std::vector<std::string> SortedVector(const std::set<std::string> &values)
{
  return {values.begin(), values.end()};
}

}  // namespace

nlohmann::json DefaultDocumentationGovernance()
{
  nlohmann::json collections = nlohmann::json::array();
  for (const DocumentationCollection &collection : CollectionList())
  {
    collections.push_back(CollectionJson(collection));
  }

  return {
      {"governance_id", "styio-docs"},
      {"branch_policy", {{"single_branch", "stable"}, {"docs_changes_follow_code_changes", true}}},
      {"single_sources_of_truth",
       {"native JSON contracts", "business manifests", "generated docs indexes", "team runbooks"}},
      {"collections", collections},
      {"ecosystem_boundaries", EcosystemDocsBoundaries()},
      {"invariants",
       {"every public behavior change has a contract, governance doc, or runbook owner",
        "generated INDEX.md files are derived artifacts and never own policy",
        "team runbooks record ownership while DOC-STATS records their review footprint",
        "documentation gates run before claiming release or audit closure"}},
  };
}

nlohmann::json BuildDocumentationChangePlan(const nlohmann::json &request)
{
  if (!request.is_object())
  {
    throw std::invalid_argument("request body must be an object");
  }
  if (!request.contains("changed_paths") || !request["changed_paths"].is_array())
  {
    throw std::invalid_argument("changed_paths must be an array");
  }

  std::set<std::string> touched_collections;
  std::set<std::string> required_gates;
  std::set<std::string> required_runbooks;
  nlohmann::json path_impacts = nlohmann::json::array();

  for (const nlohmann::json &entry : request["changed_paths"])
  {
    if (!entry.is_string() || entry.get<std::string>().empty())
    {
      throw std::invalid_argument("changed_paths entries must be non-empty strings");
    }
    const std::string path = entry.get<std::string>();
    if (path.find("..") != std::string::npos || StartsWith(path, "/"))
    {
      throw std::invalid_argument("changed_paths entries must be repository-relative paths");
    }
    const std::string collection_id = CollectionForPath(path);
    touched_collections.insert(collection_id);
    const std::vector<std::string> gates = GatesForCollection(collection_id);
    const std::vector<std::string> runbooks = RunbooksForPath(path);
    AddAll(required_gates, gates);
    AddAll(required_runbooks, runbooks);
    path_impacts.push_back({
        {"path", path},
        {"collection_id", collection_id},
        {"required_gates", gates},
        {"required_runbooks", runbooks},
    });
  }

  if (!required_runbooks.empty())
  {
    required_gates.insert("team-docs-gate");
    required_runbooks.insert("docs/teams/DOC-STATS.md");
  }
  required_gates.insert("git-diff-check");

  return {
      {"governance_id", "styio-docs"},
      {"repository_id", request.value("repository_id", std::string("styio-platform"))},
      {"change_kind", request.value("change_kind", std::string("documentation"))},
      {"branch", "stable"},
      {"path_impacts", path_impacts},
      {"touched_collections", SortedVector(touched_collections)},
      {"required_runbooks", SortedVector(required_runbooks)},
      {"required_gates", SortedVector(required_gates)},
      {"review_policy",
       {{"generated_indexes", "refresh with python3 scripts/docs-index.py --write"},
        {"runbook_stats", "refresh docs/teams/DOC-STATS.md when team runbooks change"},
        {"contracts", "update native JSON contract examples before route docs claim closure"}}},
  };
}

}  // namespace spio::platform
