#include "PlatformStorage/PlatformPersistence/PostgresStore.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

#if STYIO_PLATFORM_HAS_LIBPQ
#include <libpq-fe.h>
#endif

namespace pafio::platform
{

namespace
{

std::string JsonText(const nlohmann::json &value)
{
  return value.dump();
}

std::string UtcTimestampSql(std::string_view column)
{
  return "to_char(" + std::string(column) + " AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')";
}

#if STYIO_PLATFORM_HAS_LIBPQ

class PgResult
{
public:
  explicit PgResult(PGresult *result) : result_(result) {}
  ~PgResult()
  {
    if (result_ != nullptr)
    {
      PQclear(result_);
    }
  }

  PgResult(const PgResult &) = delete;
  PgResult &operator=(const PgResult &) = delete;
  PgResult(PgResult &&other) noexcept : result_(std::exchange(other.result_, nullptr)) {}
  PgResult &operator=(PgResult &&other) noexcept
  {
    if (this != &other)
    {
      if (result_ != nullptr)
      {
        PQclear(result_);
      }
      result_ = std::exchange(other.result_, nullptr);
    }
    return *this;
  }

  PGresult *get() const { return result_; }

private:
  PGresult *result_ = nullptr;
};

class PgConnection
{
public:
  explicit PgConnection(const std::string &dsn) : connection_(PQconnectdb(dsn.c_str()))
  {
    if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK)
    {
      const std::string detail = connection_ == nullptr ? "connection allocation failed" : PQerrorMessage(connection_);
      if (connection_ != nullptr)
      {
        PQfinish(connection_);
        connection_ = nullptr;
      }
      throw PostgresStoreError("postgres connection failed: " + detail);
    }
  }

  ~PgConnection()
  {
    if (connection_ != nullptr)
    {
      PQfinish(connection_);
    }
  }

  PgConnection(const PgConnection &) = delete;
  PgConnection &operator=(const PgConnection &) = delete;

  void Exec(const std::string &sql) const
  {
    PgResult result(PQexec(connection_, sql.c_str()));
    const ExecStatusType status = PQresultStatus(result.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
    {
      throw PostgresStoreError("postgres command failed: " + std::string(PQerrorMessage(connection_)));
    }
  }

  PgResult ExecParams(const std::string &sql, const std::vector<std::string> &params) const
  {
    std::vector<const char *> values;
    values.reserve(params.size());
    for (const std::string &param : params)
    {
      values.push_back(param.c_str());
    }
    PgResult result(PQexecParams(
        connection_,
        sql.c_str(),
        static_cast<int>(params.size()),
        nullptr,
        values.data(),
        nullptr,
        nullptr,
        0));
    const ExecStatusType status = PQresultStatus(result.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
    {
      throw PostgresStoreError("postgres command failed: " + std::string(PQerrorMessage(connection_)));
    }
    return result;
  }

private:
  PGconn *connection_ = nullptr;
};

int ColumnIndex(PGresult *result, const char *name)
{
  const int index = PQfnumber(result, name);
  if (index < 0)
  {
    throw PostgresStoreError(std::string("postgres result missing column: ") + name);
  }
  return index;
}

std::string ColumnText(PGresult *result, int row, const char *name)
{
  const int column = ColumnIndex(result, name);
  if (PQgetisnull(result, row, column) != 0)
  {
    return "";
  }
  return PQgetvalue(result, row, column);
}

nlohmann::json ColumnJson(PGresult *result, int row, const char *name)
{
  const std::string text = ColumnText(result, row, name);
  if (text.empty())
  {
    return nlohmann::json::object();
  }
  return nlohmann::json::parse(text);
}

PlatformJobRecord LoadJobRow(PGresult *result, int row)
{
  PlatformJobRecord job;
  job.job_id = ColumnText(result, row, "job_id");
  job.tenant_id = ColumnText(result, row, "tenant_id");
  job.user_id = ColumnText(result, row, "user_id");
  job.workspace_id = ColumnText(result, row, "workspace_id");
  job.action = ColumnText(result, row, "action");
  job.status = ColumnText(result, row, "status");
  job.region = ColumnText(result, row, "region");
  job.worker_pool_key = ColumnText(result, row, "worker_pool_key");
  job.worker_id = ColumnText(result, row, "worker_id");
  job.created_at = ColumnText(result, row, "created_at_text");
  job.finished_at = ColumnText(result, row, "finished_at_text");
  job.job_request = ColumnJson(result, row, "request_text");
  return job;
}

std::string JobSelectSql(std::string_view where_clause)
{
  std::ostringstream sql;
  sql
      << "SELECT job_id, tenant_id, user_id, workspace_id, action, status, region, worker_pool_key, "
      << "worker_id, request::text AS request_text, "
      << UtcTimestampSql("created_at") << " AS created_at_text, "
      << "COALESCE(" << UtcTimestampSql("finished_at") << ", '') AS finished_at_text "
      << "FROM platform_jobs " << where_clause;
  return sql.str();
}

void LoadArtifacts(PgConnection &connection, PlatformJobRecord &job)
{
  PgResult result = connection.ExecParams(
      "SELECT artifact_id, object_key, kind FROM platform_artifacts WHERE job_id = $1 ORDER BY artifact_id ASC",
      {job.job_id});
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    job.artifacts.push_back({
        .artifact_id = ColumnText(result.get(), row, "artifact_id"),
        .object_key = ColumnText(result.get(), row, "object_key"),
        .kind = ColumnText(result.get(), row, "kind"),
    });
  }
}

CompileContainerRecord LoadCompileContainerRow(PGresult *result, int row)
{
  CompileContainerRecord container;
  container.container_id = ColumnText(result, row, "container_id");
  container.worker_id = ColumnText(result, row, "worker_id");
  container.tenant_id = ColumnText(result, row, "tenant_id");
  container.user_id = ColumnText(result, row, "user_id");
  container.current_workspace_id = ColumnText(result, row, "current_workspace_id");
  container.region = ColumnText(result, row, "region");
  container.worker_pool_key = ColumnText(result, row, "worker_pool_key");
  container.status = ColumnText(result, row, "status");
  container.capacity = std::stoi(ColumnText(result, row, "capacity"));
  container.workspace_generation = std::stoi(ColumnText(result, row, "workspace_generation"));
  container.created_at = ColumnText(result, row, "created_at_text");
  container.last_switched_at = ColumnText(result, row, "last_switched_at_text");
  container.last_switch_reason = ColumnText(result, row, "last_switch_reason");
  return container;
}

std::string CompileContainerSelectSql(std::string_view where_clause)
{
  std::ostringstream sql;
  sql
      << "SELECT container_id, worker_id, tenant_id, user_id, current_workspace_id, region, worker_pool_key, "
      << "status, capacity::text AS capacity, workspace_generation::text AS workspace_generation, "
      << UtcTimestampSql("created_at") << " AS created_at_text, "
      << "COALESCE(" << UtcTimestampSql("last_switched_at") << ", '') AS last_switched_at_text, "
      << "COALESCE(last_switch_reason, '') AS last_switch_reason "
      << "FROM platform_compile_containers " << where_clause;
  return sql.str();
}

RegistryPackageRecord LoadRegistryPackageRow(PGresult *result, int row)
{
  return {
      .package_id = ColumnText(result, row, "package_id"),
      .package_namespace = ColumnText(result, row, "namespace"),
      .name = ColumnText(result, row, "name"),
      .created_at = ColumnText(result, row, "created_at_text"),
      .created_by = ColumnText(result, row, "created_by"),
      .visibility = ColumnText(result, row, "visibility"),
  };
}

std::string RegistryPackageSelectSql(std::string_view where_clause)
{
  std::ostringstream sql;
  sql << "SELECT package_id, namespace, name, " << UtcTimestampSql("created_at") << " AS created_at_text, "
      << "created_by, visibility FROM platform_packages " << where_clause;
  return sql.str();
}

RegistryPackageReleaseRecord LoadRegistryPackageReleaseRow(PGresult *result, int row)
{
  return {
      .package_id = ColumnText(result, row, "package_id"),
      .version = ColumnText(result, row, "version"),
      .edition = ColumnText(result, row, "edition"),
      .manifest_sha256 = ColumnText(result, row, "manifest_sha256"),
      .source_artifact_sha256 = ColumnText(result, row, "source_artifact_sha256"),
      .dependencies = nlohmann::json::parse(ColumnText(result, row, "dependencies_text")),
      .publisher_id = ColumnText(result, row, "publisher_id"),
      .published_at = ColumnText(result, row, "published_at_text"),
      .yanked = ColumnText(result, row, "yanked") == "true" || ColumnText(result, row, "yanked") == "t",
      .yanked_reason = ColumnText(result, row, "yanked_reason"),
  };
}

std::string RegistryReleaseSelectSql(std::string_view where_clause)
{
  std::ostringstream sql;
  sql << "SELECT package_id, version, edition, manifest_sha256, source_artifact_sha256, "
      << "dependencies::text AS dependencies_text, publisher_id, "
      << UtcTimestampSql("published_at") << " AS published_at_text, yanked::text AS yanked, yanked_reason "
      << "FROM platform_package_releases " << where_clause;
  return sql.str();
}

RegistryPackageOwnerRecord LoadRegistryPackageOwnerRow(PGresult *result, int row)
{
  return {
      .package_id = ColumnText(result, row, "package_id"),
      .owner_id = ColumnText(result, row, "owner_id"),
      .owner_kind = ColumnText(result, row, "owner_kind"),
      .role = ColumnText(result, row, "role"),
      .added_by = ColumnText(result, row, "added_by"),
      .added_at = ColumnText(result, row, "added_at_text"),
  };
}

RegistryPublishTokenRecord LoadRegistryPublishTokenRow(PGresult *result, int row)
{
  return {
      .token_id = ColumnText(result, row, "token_id"),
      .token_hash = ColumnText(result, row, "token_hash"),
      .owner_id = ColumnText(result, row, "owner_id"),
      .scopes = nlohmann::json::parse(ColumnText(result, row, "scopes_text")).get<std::vector<std::string>>(),
      .package_patterns = nlohmann::json::parse(ColumnText(result, row, "package_patterns_text")).get<std::vector<std::string>>(),
      .expires_at = ColumnText(result, row, "expires_at_text"),
      .revoked_at = ColumnText(result, row, "revoked_at_text"),
      .created_at = ColumnText(result, row, "created_at_text"),
  };
}

std::string RegistryTokenSelectSql(std::string_view where_clause)
{
  std::ostringstream sql;
  sql << "SELECT token_id, token_hash, owner_id, scopes::text AS scopes_text, "
      << "package_patterns::text AS package_patterns_text, "
      << "COALESCE(" << UtcTimestampSql("expires_at") << ", '') AS expires_at_text, "
      << "COALESCE(" << UtcTimestampSql("revoked_at") << ", '') AS revoked_at_text, "
      << UtcTimestampSql("created_at") << " AS created_at_text "
      << "FROM platform_publish_tokens " << where_clause;
  return sql.str();
}

RegistryRepositoryRecord LoadRegistryRepositoryRow(PGresult *result, int row)
{
  return {
      .repository_id = ColumnText(result, row, "repository_id"),
      .name = ColumnText(result, row, "name"),
      .tenant_id = ColumnText(result, row, "tenant_id"),
      .policy = nlohmann::json::parse(ColumnText(result, row, "policy_text")),
      .created_at = ColumnText(result, row, "created_at_text"),
  };
}

RegistryRepositoryVersionRecord LoadRegistryRepositoryVersionRow(PGresult *result, int row)
{
  return {
      .repository_version_id = ColumnText(result, row, "repository_version_id"),
      .repository_id = ColumnText(result, row, "repository_id"),
      .sequence = std::stoi(ColumnText(result, row, "sequence")),
      .change_kind = ColumnText(result, row, "change_kind"),
      .change_ref = ColumnText(result, row, "change_ref"),
      .created_at = ColumnText(result, row, "created_at_text"),
  };
}

RegistryPublicationRecord LoadRegistryPublicationRow(PGresult *result, int row)
{
  return {
      .publication_id = ColumnText(result, row, "publication_id"),
      .repository_version_id = ColumnText(result, row, "repository_version_id"),
      .layout_version = std::stoi(ColumnText(result, row, "layout_version")),
      .root_path = ColumnText(result, row, "root_path"),
      .manifest_sha256 = ColumnText(result, row, "manifest_sha256"),
      .tree_size = std::stoi(ColumnText(result, row, "tree_size")),
      .created_at = ColumnText(result, row, "created_at_text"),
      .verified = ColumnText(result, row, "verified") == "true" || ColumnText(result, row, "verified") == "t",
  };
}

RegistryDistributionRecord LoadRegistryDistributionRow(PGresult *result, int row)
{
  return {
      .distribution_id = ColumnText(result, row, "distribution_id"),
      .repository_id = ColumnText(result, row, "repository_id"),
      .name = ColumnText(result, row, "name"),
      .base_url = ColumnText(result, row, "base_url"),
      .current_publication_id = ColumnText(result, row, "current_publication_id"),
      .previous_publication_id = ColumnText(result, row, "previous_publication_id"),
      .updated_at = ColumnText(result, row, "updated_at_text"),
  };
}

RegistryAuditEventRecord LoadRegistryAuditEventRow(PGresult *result, int row)
{
  return {
      .event_id = ColumnText(result, row, "event_id"),
      .actor_id = ColumnText(result, row, "actor_id"),
      .operation = ColumnText(result, row, "operation"),
      .target = nlohmann::json::parse(ColumnText(result, row, "target_text")),
      .request_id = ColumnText(result, row, "request_id"),
      .result = ColumnText(result, row, "result"),
      .created_at = ColumnText(result, row, "created_at_text"),
  };
}

#endif

}  // namespace

bool LooksLikePostgresDsn(std::string_view value)
{
  return value.starts_with("postgres://") || value.starts_with("postgresql://") || value.starts_with("host=");
}

bool PostgresDriverAvailable()
{
#if STYIO_PLATFORM_HAS_LIBPQ
  return true;
#else
  return false;
#endif
}

std::vector<SqlMigration> CloudKernelMigrations()
{
  return {
      {
          "001_cloud_kernel",
          R"SQL(
CREATE TABLE IF NOT EXISTS platform_tenants (
  tenant_id TEXT PRIMARY KEY,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_nodes (
  node_id TEXT PRIMARY KEY,
  region TEXT NOT NULL,
  roles JSONB NOT NULL,
  last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_workers (
  worker_id TEXT PRIMARY KEY,
  region TEXT NOT NULL,
  worker_pool_key TEXT NOT NULL,
  status TEXT NOT NULL,
  capacity INTEGER NOT NULL,
  last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_compile_containers (
  container_id TEXT PRIMARY KEY,
  worker_id TEXT NOT NULL REFERENCES platform_workers(worker_id),
  tenant_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  current_workspace_id TEXT NOT NULL,
  region TEXT NOT NULL,
  worker_pool_key TEXT NOT NULL,
  status TEXT NOT NULL,
  capacity INTEGER NOT NULL,
  workspace_generation INTEGER NOT NULL DEFAULT 1,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_switched_at TIMESTAMPTZ,
  last_switch_reason TEXT
);

CREATE INDEX IF NOT EXISTS platform_compile_containers_worker_idx
  ON platform_compile_containers (worker_id, region, worker_pool_key, status);

CREATE INDEX IF NOT EXISTS platform_compile_containers_binding_idx
  ON platform_compile_containers (tenant_id, user_id, status);

CREATE SEQUENCE IF NOT EXISTS platform_job_id_seq;

CREATE TABLE IF NOT EXISTS platform_jobs (
  job_id TEXT PRIMARY KEY,
  tenant_id TEXT NOT NULL REFERENCES platform_tenants(tenant_id),
  user_id TEXT NOT NULL DEFAULT 'unbound-user',
  workspace_id TEXT NOT NULL,
  action TEXT NOT NULL,
  status TEXT NOT NULL,
  region TEXT NOT NULL,
  worker_pool_key TEXT NOT NULL,
  worker_id TEXT,
  request JSONB NOT NULL,
  result JSONB,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  finished_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS platform_jobs_claim_idx
  ON platform_jobs (status, region, worker_pool_key, created_at);

CREATE INDEX IF NOT EXISTS platform_jobs_claim_user_idx
  ON platform_jobs (status, region, worker_pool_key, tenant_id, user_id, created_at);

CREATE TABLE IF NOT EXISTS platform_job_events (
  event_id BIGSERIAL PRIMARY KEY,
  job_id TEXT NOT NULL REFERENCES platform_jobs(job_id),
  status TEXT NOT NULL,
  message TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_artifacts (
  artifact_id TEXT PRIMARY KEY,
  job_id TEXT NOT NULL REFERENCES platform_jobs(job_id),
  object_key TEXT NOT NULL,
  kind TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_mirror_cursors (
  mirror_id TEXT PRIMARY KEY,
  region TEXT NOT NULL,
  origin TEXT NOT NULL,
  freshness TEXT NOT NULL,
  replay_cursor TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_workgroup_clusters (
  workgroup_id TEXT NOT NULL,
  cluster_id TEXT NOT NULL,
  region TEXT NOT NULL,
  node_id TEXT NOT NULL,
  control_plane_endpoint TEXT NOT NULL,
  registry_endpoint TEXT,
  mirror_endpoint TEXT,
  internal_control_plane_endpoint TEXT,
  roles JSONB NOT NULL,
  labels JSONB NOT NULL,
  trust_domain TEXT NOT NULL,
  registration_policy TEXT NOT NULL,
  status TEXT NOT NULL,
  registered_by JSONB NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (workgroup_id, cluster_id)
);

CREATE INDEX IF NOT EXISTS platform_workgroup_clusters_region_idx
  ON platform_workgroup_clusters (workgroup_id, region, status);
)SQL",
      },
      {
          "002_user_bound_compile_containers",
          R"SQL(
ALTER TABLE platform_jobs
  ADD COLUMN IF NOT EXISTS user_id TEXT NOT NULL DEFAULT 'unbound-user';

CREATE TABLE IF NOT EXISTS platform_compile_containers (
  container_id TEXT PRIMARY KEY,
  worker_id TEXT NOT NULL REFERENCES platform_workers(worker_id),
  tenant_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  current_workspace_id TEXT NOT NULL,
  region TEXT NOT NULL,
  worker_pool_key TEXT NOT NULL,
  status TEXT NOT NULL,
  capacity INTEGER NOT NULL,
  workspace_generation INTEGER NOT NULL DEFAULT 1,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_switched_at TIMESTAMPTZ,
  last_switch_reason TEXT
);

CREATE INDEX IF NOT EXISTS platform_compile_containers_worker_idx
  ON platform_compile_containers (worker_id, region, worker_pool_key, status);

CREATE INDEX IF NOT EXISTS platform_compile_containers_binding_idx
  ON platform_compile_containers (tenant_id, user_id, status);

CREATE INDEX IF NOT EXISTS platform_jobs_claim_user_idx
  ON platform_jobs (status, region, worker_pool_key, tenant_id, user_id, created_at);
)SQL",
      },
      {
          "003_package_registry_repository_model",
          R"SQL(
ALTER TABLE platform_mirror_cursors
  ADD COLUMN IF NOT EXISTS publication_id TEXT NOT NULL DEFAULT '',
  ADD COLUMN IF NOT EXISTS repository_version_id TEXT NOT NULL DEFAULT '',
  ADD COLUMN IF NOT EXISTS synced_at TIMESTAMPTZ,
  ADD COLUMN IF NOT EXISTS tree_size INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS platform_packages (
  package_id TEXT PRIMARY KEY,
  namespace TEXT NOT NULL,
  name TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  created_by TEXT NOT NULL,
  visibility TEXT NOT NULL,
  UNIQUE(namespace, name)
);

CREATE TABLE IF NOT EXISTS platform_package_releases (
  package_id TEXT NOT NULL REFERENCES platform_packages(package_id),
  version TEXT NOT NULL,
  edition TEXT NOT NULL,
  manifest_sha256 TEXT NOT NULL,
  source_artifact_sha256 TEXT NOT NULL,
  dependencies JSONB NOT NULL,
  publisher_id TEXT NOT NULL,
  published_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  yanked BOOLEAN NOT NULL DEFAULT false,
  yanked_reason TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (package_id, version)
);

CREATE TABLE IF NOT EXISTS platform_package_owners (
  package_id TEXT NOT NULL REFERENCES platform_packages(package_id),
  owner_id TEXT NOT NULL,
  owner_kind TEXT NOT NULL,
  role TEXT NOT NULL,
  added_by TEXT NOT NULL,
  added_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (package_id, owner_id)
);

CREATE TABLE IF NOT EXISTS platform_publish_tokens (
  token_id TEXT PRIMARY KEY,
  token_hash TEXT NOT NULL UNIQUE,
  owner_id TEXT NOT NULL,
  scopes JSONB NOT NULL,
  package_patterns JSONB NOT NULL,
  expires_at TIMESTAMPTZ,
  revoked_at TIMESTAMPTZ,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE SEQUENCE IF NOT EXISTS platform_publish_token_id_seq;

CREATE TABLE IF NOT EXISTS platform_repositories (
  repository_id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  tenant_id TEXT NOT NULL,
  policy JSONB NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_repository_versions (
  repository_version_id TEXT PRIMARY KEY,
  repository_id TEXT NOT NULL REFERENCES platform_repositories(repository_id),
  sequence INTEGER NOT NULL,
  change_kind TEXT NOT NULL,
  change_ref TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(repository_id, sequence)
);

CREATE TABLE IF NOT EXISTS platform_publications (
  publication_id TEXT PRIMARY KEY,
  repository_version_id TEXT NOT NULL REFERENCES platform_repository_versions(repository_version_id),
  layout_version INTEGER NOT NULL,
  root_path TEXT NOT NULL,
  manifest_sha256 TEXT NOT NULL,
  tree_size INTEGER NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  verified BOOLEAN NOT NULL DEFAULT false
);

CREATE TABLE IF NOT EXISTS platform_distributions (
  distribution_id TEXT PRIMARY KEY,
  repository_id TEXT NOT NULL REFERENCES platform_repositories(repository_id),
  name TEXT NOT NULL,
  base_url TEXT NOT NULL,
  current_publication_id TEXT,
  previous_publication_id TEXT,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS platform_registry_audit_events (
  event_id TEXT PRIMARY KEY,
  actor_id TEXT NOT NULL,
  operation TEXT NOT NULL,
  target JSONB NOT NULL,
  request_id TEXT NOT NULL,
  result TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE SEQUENCE IF NOT EXISTS platform_registry_audit_event_id_seq;
)SQL",
      },
  };
}

std::string ClaimJobSql()
{
  return R"SQL(
WITH candidate AS (
  SELECT job_id
  FROM platform_jobs
  WHERE status = 'queued'
    AND region = $1
    AND worker_pool_key = $2
  ORDER BY created_at ASC
  LIMIT 1
  FOR UPDATE SKIP LOCKED
)
UPDATE platform_jobs
SET status = 'running',
    worker_id = $3,
    updated_at = now()
WHERE job_id IN (SELECT job_id FROM candidate)
RETURNING *;
)SQL";
}

std::string CompleteJobSql()
{
  return R"SQL(
UPDATE platform_jobs
SET status = $2,
    result = $3::jsonb,
    updated_at = now(),
    finished_at = now()
WHERE job_id = $1
  AND worker_id = $4
  AND status = 'running'
RETURNING *;
)SQL";
}

PostgresStore::PostgresStore(std::string dsn) : dsn_(std::move(dsn)) {}

void PostgresStore::ApplyMigrations() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  for (const SqlMigration &migration : CloudKernelMigrations())
  {
    connection.Exec(migration.sql);
  }
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::UpsertNode(const PlatformConfig &config) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_nodes (node_id, region, roles, last_seen_at) "
      "VALUES ($1, $2, $3::jsonb, now()) "
      "ON CONFLICT (node_id) DO UPDATE SET region = EXCLUDED.region, roles = EXCLUDED.roles, last_seen_at = now()",
      {config.node_id, config.region, JsonText(config.roles)});
#else
  (void) config;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::string PostgresStore::NextJobId() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams("SELECT nextval('platform_job_id_seq')::text AS job_id_seq", {});
  if (PQntuples(result.get()) == 0)
  {
    throw PostgresStoreError("postgres did not return a job id sequence value");
  }
  const std::string value = ColumnText(result.get(), 0, "job_id_seq");
  return "job-" + std::string(12U - std::min<size_t>(12U, value.size()), '0') + value;
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::SubmitJob(const PlatformJobRecord &job) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.Exec("BEGIN");
  try
  {
    connection.ExecParams(
        "INSERT INTO platform_tenants (tenant_id) VALUES ($1) ON CONFLICT (tenant_id) DO NOTHING",
        {job.tenant_id});
    connection.ExecParams(
        "INSERT INTO platform_jobs "
        "(job_id, tenant_id, user_id, workspace_id, action, status, region, worker_pool_key, request, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, now()) "
        "ON CONFLICT (job_id) DO UPDATE SET status = EXCLUDED.status, worker_id = NULL, "
        "user_id = EXCLUDED.user_id, workspace_id = EXCLUDED.workspace_id, request = EXCLUDED.request, updated_at = now(), finished_at = NULL",
        {job.job_id, job.tenant_id, job.user_id, job.workspace_id, job.action, job.status, job.region, job.worker_pool_key, JsonText(job.job_request)});
    connection.ExecParams(
        "INSERT INTO platform_job_events (job_id, status, message) VALUES ($1, $2, $3)",
        {job.job_id, "queued", "job queued"});
    connection.Exec("COMMIT");
  }
  catch (...)
  {
    connection.Exec("ROLLBACK");
    throw;
  }
#else
  (void) job;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<PlatformJobRecord> PostgresStore::GetJob(const std::string &job_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(JobSelectSql("WHERE job_id = $1"), {job_id});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  PlatformJobRecord job = LoadJobRow(result.get(), 0);
  LoadArtifacts(connection, job);
  return job;
#else
  (void) job_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::vector<JobEventRecord> PostgresStore::GetJobEvents(const std::string &job_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "SELECT event_id::text, job_id, status, message, " + UtcTimestampSql("created_at") +
          " AS created_at_text FROM platform_job_events WHERE job_id = $1 ORDER BY event_id ASC",
      {job_id});
  std::vector<JobEventRecord> events;
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    events.push_back({
        .event_id = ColumnText(result.get(), row, "event_id"),
        .job_id = ColumnText(result.get(), row, "job_id"),
        .status = ColumnText(result.get(), row, "status"),
        .message = ColumnText(result.get(), row, "message"),
        .created_at = ColumnText(result.get(), row, "created_at_text"),
    });
  }
  return events;
#else
  (void) job_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<PlatformJobRecord> PostgresStore::CancelJob(const std::string &job_id, const std::string &reason) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.Exec("BEGIN");
  try
  {
    PgResult updated = connection.ExecParams(
        "UPDATE platform_jobs SET status = 'cancelled', updated_at = now(), finished_at = now() "
        "WHERE job_id = $1 AND status IN ('queued', 'running') RETURNING job_id",
        {job_id});
    if (PQntuples(updated.get()) == 0)
    {
      connection.Exec("ROLLBACK");
      return std::nullopt;
    }
    connection.ExecParams(
        "INSERT INTO platform_job_events (job_id, status, message) VALUES ($1, $2, $3)",
        {job_id, "cancelled", reason});
    connection.Exec("COMMIT");
  }
  catch (...)
  {
    connection.Exec("ROLLBACK");
    throw;
  }
  return GetJob(job_id);
#else
  (void) job_id;
  (void) reason;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::RegisterWorker(const nlohmann::json &worker) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_workers (worker_id, region, worker_pool_key, status, capacity, last_seen_at) "
      "VALUES ($1, $2, $3, $4, $5::int, now()) "
      "ON CONFLICT (worker_id) DO UPDATE SET region = EXCLUDED.region, worker_pool_key = EXCLUDED.worker_pool_key, "
      "status = EXCLUDED.status, capacity = EXCLUDED.capacity, last_seen_at = now()",
      {
          worker.at("worker_id").get<std::string>(),
          worker.at("region").get<std::string>(),
          worker.at("worker_pool_key").get<std::string>(),
          worker.value("status", "registered"),
          std::to_string(worker.value("capacity", 1)),
      });
  return worker;
#else
  (void) worker;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

CompileContainerRecord PostgresStore::RegisterCompileContainer(const CompileContainerRecord &container) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.Exec("BEGIN");
  try
  {
    PgResult registered = connection.ExecParams(
        "SELECT worker_id FROM platform_workers WHERE worker_id = $1 AND region = $2 AND worker_pool_key = $3",
        {container.worker_id, container.region, container.worker_pool_key});
    if (PQntuples(registered.get()) == 0)
    {
      throw PostgresStoreError("worker is not registered");
    }
    PgResult existing = connection.ExecParams(
        "SELECT worker_id, tenant_id, user_id FROM platform_compile_containers WHERE container_id = $1 FOR UPDATE",
        {container.container_id});
    if (PQntuples(existing.get()) > 0 &&
        (ColumnText(existing.get(), 0, "tenant_id") != container.tenant_id ||
         ColumnText(existing.get(), 0, "user_id") != container.user_id))
    {
      throw PostgresStoreError("compile container user binding mismatch");
    }
    if (PQntuples(existing.get()) > 0 && ColumnText(existing.get(), 0, "worker_id") != container.worker_id)
    {
      throw PostgresStoreError("compile container worker owner mismatch");
    }
    connection.ExecParams(
        "INSERT INTO platform_tenants (tenant_id) VALUES ($1) ON CONFLICT (tenant_id) DO NOTHING",
        {container.tenant_id});
    connection.ExecParams(
        "INSERT INTO platform_compile_containers "
        "(container_id, worker_id, tenant_id, user_id, current_workspace_id, region, worker_pool_key, status, capacity, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::int, now()) "
        "ON CONFLICT (container_id) DO UPDATE SET worker_id = EXCLUDED.worker_id, "
        "current_workspace_id = EXCLUDED.current_workspace_id, region = EXCLUDED.region, "
        "worker_pool_key = EXCLUDED.worker_pool_key, status = EXCLUDED.status, capacity = EXCLUDED.capacity, updated_at = now()",
        {
            container.container_id,
            container.worker_id,
            container.tenant_id,
            container.user_id,
            container.current_workspace_id,
            container.region,
            container.worker_pool_key,
            container.status,
            std::to_string(container.capacity),
        });
    connection.Exec("COMMIT");
  }
  catch (...)
  {
    connection.Exec("ROLLBACK");
    throw;
  }
  const std::optional<CompileContainerRecord> saved = GetCompileContainer(container.container_id);
  if (!saved.has_value())
  {
    throw PostgresStoreError("compile container registration did not return a row");
  }
  return *saved;
#else
  (void) container;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<CompileContainerRecord> PostgresStore::GetCompileContainer(const std::string &container_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(CompileContainerSelectSql("WHERE container_id = $1"), {container_id});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return LoadCompileContainerRow(result.get(), 0);
#else
  (void) container_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<CompileContainerRecord> PostgresStore::SwitchCompileContainerWorkspace(
    const std::string &container_id,
    const std::string &worker_id,
    const std::string &workspace_id,
    const std::string &reason) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "UPDATE platform_compile_containers "
      "SET workspace_generation = CASE WHEN current_workspace_id = $3 THEN workspace_generation ELSE workspace_generation + 1 END, "
      "current_workspace_id = $3, "
      "last_switched_at = CASE WHEN current_workspace_id = $3 THEN last_switched_at ELSE now() END, "
      "last_switch_reason = CASE WHEN current_workspace_id = $3 THEN last_switch_reason ELSE $4 END, "
      "updated_at = now() "
      "WHERE container_id = $1 AND worker_id = $2 AND status = 'active' "
      "RETURNING container_id",
      {container_id, worker_id, workspace_id, reason});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return GetCompileContainer(container_id);
#else
  (void) container_id;
  (void) worker_id;
  (void) workspace_id;
  (void) reason;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<PlatformJobRecord> PostgresStore::ClaimJob(
    const std::string &worker_id,
    const std::string &region,
    const std::string &worker_pool_key) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.Exec("BEGIN");
  std::string job_id;
  try
  {
    PgResult registered = connection.ExecParams(
        "SELECT worker_id FROM platform_workers WHERE worker_id = $1 AND region = $2 AND worker_pool_key = $3",
        {worker_id, region, worker_pool_key});
    if (PQntuples(registered.get()) == 0)
    {
      throw PostgresStoreError("worker is not registered");
    }
    PgResult claimed = connection.ExecParams(ClaimJobSql(), {region, worker_pool_key, worker_id});
    if (PQntuples(claimed.get()) == 0)
    {
      connection.Exec("COMMIT");
      return std::nullopt;
    }
    job_id = ColumnText(claimed.get(), 0, "job_id");
    connection.ExecParams(
        "INSERT INTO platform_job_events (job_id, status, message) VALUES ($1, $2, $3)",
        {job_id, "running", "job claimed by worker"});
    connection.Exec("COMMIT");
  }
  catch (...)
  {
    connection.Exec("ROLLBACK");
    throw;
  }
  return GetJob(job_id);
#else
  (void) worker_id;
  (void) region;
  (void) worker_pool_key;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<PlatformJobRecord> PostgresStore::ClaimJobForCompileContainer(
    const std::string &worker_id,
    const std::string &region,
    const std::string &worker_pool_key,
    const std::string &container_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.Exec("BEGIN");
  std::string job_id;
  try
  {
    PgResult registered = connection.ExecParams(
        "SELECT worker_id FROM platform_workers WHERE worker_id = $1 AND region = $2 AND worker_pool_key = $3",
        {worker_id, region, worker_pool_key});
    if (PQntuples(registered.get()) == 0)
    {
      throw PostgresStoreError("worker is not registered");
    }

    PgResult container_result = connection.ExecParams(
        CompileContainerSelectSql("WHERE container_id = $1 AND worker_id = $2 AND region = $3 AND worker_pool_key = $4 AND status = 'active' FOR UPDATE"),
        {container_id, worker_id, region, worker_pool_key});
    if (PQntuples(container_result.get()) == 0)
    {
      throw PostgresStoreError("compile container is not registered");
    }
    const CompileContainerRecord container = LoadCompileContainerRow(container_result.get(), 0);

    PgResult claimed = connection.ExecParams(
        R"SQL(
WITH candidate AS (
  SELECT job_id
  FROM platform_jobs
  WHERE status = 'queued'
    AND region = $1
    AND worker_pool_key = $2
    AND tenant_id = $4
    AND user_id = $5
  ORDER BY created_at ASC
  LIMIT 1
  FOR UPDATE SKIP LOCKED
)
UPDATE platform_jobs
SET status = 'running',
    worker_id = $3,
    updated_at = now()
WHERE job_id IN (SELECT job_id FROM candidate)
RETURNING job_id, workspace_id;
)SQL",
        {region, worker_pool_key, worker_id, container.tenant_id, container.user_id});
    if (PQntuples(claimed.get()) == 0)
    {
      connection.Exec("COMMIT");
      return std::nullopt;
    }

    job_id = ColumnText(claimed.get(), 0, "job_id");
    const std::string workspace_id = ColumnText(claimed.get(), 0, "workspace_id");
    if (workspace_id != container.current_workspace_id)
    {
      connection.ExecParams(
          "UPDATE platform_compile_containers "
          "SET current_workspace_id = $3, workspace_generation = workspace_generation + 1, "
          "last_switched_at = now(), last_switch_reason = 'claimJob', updated_at = now() "
          "WHERE container_id = $1 AND worker_id = $2",
          {container_id, worker_id, workspace_id});
      connection.ExecParams(
          "INSERT INTO platform_job_events (job_id, status, message) VALUES ($1, $2, $3)",
          {job_id, "running", "compile container switched workspace"});
    }
    connection.ExecParams(
        "INSERT INTO platform_job_events (job_id, status, message) VALUES ($1, $2, $3)",
        {job_id, "running", "job claimed by compile container"});
    connection.Exec("COMMIT");
  }
  catch (...)
  {
    connection.Exec("ROLLBACK");
    throw;
  }
  return GetJob(job_id);
#else
  (void) worker_id;
  (void) region;
  (void) worker_pool_key;
  (void) container_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<PlatformJobRecord> PostgresStore::HeartbeatJob(
    const std::string &job_id,
    const std::string &worker_id,
    const std::string &message) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult owned = connection.ExecParams(
      "UPDATE platform_jobs SET updated_at = now() WHERE job_id = $1 AND worker_id = $2 AND status = 'running' RETURNING job_id",
      {job_id, worker_id});
  if (PQntuples(owned.get()) == 0)
  {
    return std::nullopt;
  }
  connection.ExecParams(
      "INSERT INTO platform_job_events (job_id, status, message) VALUES ($1, $2, $3)",
      {job_id, "running", message});
  return GetJob(job_id);
#else
  (void) job_id;
  (void) worker_id;
  (void) message;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<PlatformJobRecord> PostgresStore::CompleteJob(
    const std::string &job_id,
    const std::string &worker_id,
    const std::string &status,
    const std::string &message,
    const std::vector<ArtifactRecord> &artifacts,
    const nlohmann::json &result) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.Exec("BEGIN");
  try
  {
    PgResult updated = connection.ExecParams(CompleteJobSql(), {job_id, status, JsonText(result), worker_id});
    if (PQntuples(updated.get()) == 0)
    {
      connection.Exec("ROLLBACK");
      return std::nullopt;
    }
    connection.ExecParams("DELETE FROM platform_artifacts WHERE job_id = $1", {job_id});
    for (const ArtifactRecord &artifact : artifacts)
    {
      connection.ExecParams(
          "INSERT INTO platform_artifacts (artifact_id, job_id, object_key, kind) VALUES ($1, $2, $3, $4)",
          {artifact.artifact_id, job_id, artifact.object_key, artifact.kind});
    }
    connection.ExecParams(
        "INSERT INTO platform_job_events (job_id, status, message) VALUES ($1, $2, $3)",
        {job_id, status, message});
    connection.Exec("COMMIT");
  }
  catch (...)
  {
    connection.Exec("ROLLBACK");
    throw;
  }
  return GetJob(job_id);
#else
  (void) job_id;
  (void) worker_id;
  (void) status;
  (void) message;
  (void) artifacts;
  (void) result;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::RegisterWorkgroupCluster(const std::string &workgroup_id, const nlohmann::json &cluster) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_workgroup_clusters "
      "(workgroup_id, cluster_id, region, node_id, control_plane_endpoint, registry_endpoint, mirror_endpoint, "
      "internal_control_plane_endpoint, roles, labels, trust_domain, registration_policy, status, registered_by, updated_at) "
      "VALUES ($1, $2, $3, $4, $5, NULLIF($6, ''), NULLIF($7, ''), NULLIF($8, ''), "
      "$9::jsonb, $10::jsonb, $11, $12, $13, $14::jsonb, now()) "
      "ON CONFLICT (workgroup_id, cluster_id) DO UPDATE SET "
      "region = EXCLUDED.region, node_id = EXCLUDED.node_id, control_plane_endpoint = EXCLUDED.control_plane_endpoint, "
      "registry_endpoint = EXCLUDED.registry_endpoint, mirror_endpoint = EXCLUDED.mirror_endpoint, "
      "internal_control_plane_endpoint = EXCLUDED.internal_control_plane_endpoint, roles = EXCLUDED.roles, labels = EXCLUDED.labels, "
      "trust_domain = EXCLUDED.trust_domain, registration_policy = EXCLUDED.registration_policy, "
      "status = EXCLUDED.status, registered_by = EXCLUDED.registered_by, updated_at = now()",
      {
          workgroup_id,
          cluster.at("cluster_id").get<std::string>(),
          cluster.at("region").get<std::string>(),
          cluster.at("node_id").get<std::string>(),
          cluster.at("control_plane_endpoint").get<std::string>(),
          cluster.value("registry_endpoint", ""),
          cluster.value("mirror_endpoint", ""),
          cluster.value("internal_control_plane_endpoint", ""),
          JsonText(cluster.at("roles")),
          JsonText(cluster.value("labels", nlohmann::json::object())),
          cluster.at("trust_domain").get<std::string>(),
          cluster.at("registration_policy").get<std::string>(),
          cluster.value("status", "registered"),
          JsonText(cluster.value("registered_by", nlohmann::json::object())),
      });
  return cluster;
#else
  (void) workgroup_id;
  (void) cluster;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListWorkgroupClusters(const std::string &workgroup_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string(
          "SELECT workgroup_id, cluster_id, region, node_id, control_plane_endpoint, "
          "COALESCE(registry_endpoint, '') AS registry_endpoint, "
          "COALESCE(mirror_endpoint, '') AS mirror_endpoint, "
          "COALESCE(internal_control_plane_endpoint, '') AS internal_control_plane_endpoint, "
          "roles::text AS roles_text, labels::text AS labels_text, trust_domain, registration_policy, status, "
          "registered_by::text AS registered_by_text, ") +
          UtcTimestampSql("updated_at") + " AS updated_at_text "
      "FROM platform_workgroup_clusters WHERE workgroup_id = $1 ORDER BY cluster_id ASC",
      {workgroup_id});
  nlohmann::json clusters = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    nlohmann::json cluster = {
        {"workgroup_id", ColumnText(result.get(), row, "workgroup_id")},
        {"cluster_id", ColumnText(result.get(), row, "cluster_id")},
        {"region", ColumnText(result.get(), row, "region")},
        {"node_id", ColumnText(result.get(), row, "node_id")},
        {"control_plane_endpoint", ColumnText(result.get(), row, "control_plane_endpoint")},
        {"roles", nlohmann::json::parse(ColumnText(result.get(), row, "roles_text"))},
        {"labels", nlohmann::json::parse(ColumnText(result.get(), row, "labels_text"))},
        {"trust_domain", ColumnText(result.get(), row, "trust_domain")},
        {"registration_policy", ColumnText(result.get(), row, "registration_policy")},
        {"status", ColumnText(result.get(), row, "status")},
        {"registered_by", nlohmann::json::parse(ColumnText(result.get(), row, "registered_by_text"))},
        {"updated_at", ColumnText(result.get(), row, "updated_at_text")},
    };
    for (const char *field : {"registry_endpoint", "mirror_endpoint", "internal_control_plane_endpoint"})
    {
      const std::string value = ColumnText(result.get(), row, field);
      if (!value.empty())
      {
        cluster[field] = value;
      }
    }
    clusters.push_back(std::move(cluster));
  }
  return clusters;
#else
  (void) workgroup_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::RecordMirrorState(
    const std::string &mirror_id,
    const std::string &region,
    const std::string &origin,
    const std::string &freshness,
    const std::string &replay_cursor,
    const std::string &publication_id,
    const std::string &repository_version_id,
    const std::string &synced_at,
    const int tree_size) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_mirror_cursors "
      "(mirror_id, region, origin, freshness, replay_cursor, publication_id, repository_version_id, synced_at, tree_size, updated_at) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7, NULLIF($8, '')::timestamptz, $9::integer, now()) "
      "ON CONFLICT (mirror_id) DO UPDATE SET region = EXCLUDED.region, origin = EXCLUDED.origin, "
      "freshness = EXCLUDED.freshness, replay_cursor = EXCLUDED.replay_cursor, publication_id = EXCLUDED.publication_id, "
      "repository_version_id = EXCLUDED.repository_version_id, synced_at = EXCLUDED.synced_at, tree_size = EXCLUDED.tree_size, updated_at = now()",
      {mirror_id, region, origin, freshness, replay_cursor, publication_id, repository_version_id, synced_at, std::to_string(tree_size)});
#else
  (void) mirror_id;
  (void) region;
  (void) origin;
  (void) freshness;
  (void) replay_cursor;
  (void) publication_id;
  (void) repository_version_id;
  (void) synced_at;
  (void) tree_size;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<MirrorCursorRecord> PostgresStore::GetMirrorState(const std::string &mirror_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT mirror_id, region, origin, freshness, replay_cursor, publication_id, repository_version_id, ") +
          "COALESCE(" + UtcTimestampSql("synced_at") + ", '') AS synced_at_text, tree_size::text AS tree_size "
          "FROM platform_mirror_cursors WHERE mirror_id = $1",
      {mirror_id});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return MirrorCursorRecord{
      .mirror_id = ColumnText(result.get(), 0, "mirror_id"),
      .region = ColumnText(result.get(), 0, "region"),
      .origin = ColumnText(result.get(), 0, "origin"),
      .freshness = ColumnText(result.get(), 0, "freshness"),
      .replay_cursor = ColumnText(result.get(), 0, "replay_cursor"),
      .publication_id = ColumnText(result.get(), 0, "publication_id"),
      .repository_version_id = ColumnText(result.get(), 0, "repository_version_id"),
      .synced_at = ColumnText(result.get(), 0, "synced_at_text"),
      .tree_size = std::stoi(ColumnText(result.get(), 0, "tree_size")),
  };
#else
  (void) mirror_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::UpsertRegistryPackage(const RegistryPackageRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_packages (package_id, namespace, name, created_at, created_by, visibility) "
      "VALUES ($1, $2, $3, NULLIF($4, '')::timestamptz, $5, $6) "
      "ON CONFLICT (package_id) DO UPDATE SET namespace = EXCLUDED.namespace, name = EXCLUDED.name, "
      "created_by = EXCLUDED.created_by, visibility = EXCLUDED.visibility",
      {record.package_id, record.package_namespace, record.name, record.created_at, record.created_by, record.visibility});
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<RegistryPackageRecord> PostgresStore::GetRegistryPackage(const std::string &package_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(RegistryPackageSelectSql("WHERE package_id = $1"), {package_id});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return LoadRegistryPackageRow(result.get(), 0);
#else
  (void) package_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryPackages() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(RegistryPackageSelectSql("ORDER BY namespace, name"), {});
  nlohmann::json packages = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    packages.push_back(SerializeRegistryPackageRecord(LoadRegistryPackageRow(result.get(), row)));
  }
  return packages;
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::UpsertRegistryPackageRelease(const RegistryPackageReleaseRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_package_releases "
      "(package_id, version, edition, manifest_sha256, source_artifact_sha256, dependencies, publisher_id, published_at, yanked, yanked_reason) "
      "VALUES ($1, $2, $3, $4, $5, $6::jsonb, $7, NULLIF($8, '')::timestamptz, $9::boolean, $10) "
      "ON CONFLICT (package_id, version) DO UPDATE SET edition = EXCLUDED.edition, manifest_sha256 = EXCLUDED.manifest_sha256, "
      "source_artifact_sha256 = EXCLUDED.source_artifact_sha256, dependencies = EXCLUDED.dependencies, "
      "publisher_id = EXCLUDED.publisher_id, yanked = EXCLUDED.yanked, yanked_reason = EXCLUDED.yanked_reason",
      {
          record.package_id,
          record.version,
          record.edition,
          record.manifest_sha256,
          record.source_artifact_sha256,
          JsonText(record.dependencies),
          record.publisher_id,
          record.published_at,
          record.yanked ? "true" : "false",
          record.yanked_reason,
      });
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<RegistryPackageReleaseRecord> PostgresStore::GetRegistryPackageRelease(
    const std::string &package_id,
    const std::string &version) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      RegistryReleaseSelectSql("WHERE package_id = $1 AND version = $2"),
      {package_id, version});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return LoadRegistryPackageReleaseRow(result.get(), 0);
#else
  (void) package_id;
  (void) version;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryPackageReleases(const std::string &package_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      RegistryReleaseSelectSql("WHERE package_id = $1 ORDER BY published_at ASC, version ASC"),
      {package_id});
  nlohmann::json releases = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    releases.push_back(SerializeRegistryPackageReleaseRecord(LoadRegistryPackageReleaseRow(result.get(), row)));
  }
  return releases;
#else
  (void) package_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

bool PostgresStore::SetRegistryPackageReleaseYanked(
    const std::string &package_id,
    const std::string &version,
    const bool yanked,
    const std::string &reason) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "UPDATE platform_package_releases SET yanked = $3::boolean, yanked_reason = $4 "
      "WHERE package_id = $1 AND version = $2 RETURNING package_id",
      {package_id, version, yanked ? "true" : "false", reason});
  return PQntuples(result.get()) > 0;
#else
  (void) package_id;
  (void) version;
  (void) yanked;
  (void) reason;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::AddRegistryPackageOwner(const RegistryPackageOwnerRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_package_owners (package_id, owner_id, owner_kind, role, added_by, added_at) "
      "VALUES ($1, $2, $3, $4, $5, NULLIF($6, '')::timestamptz) "
      "ON CONFLICT (package_id, owner_id) DO UPDATE SET owner_kind = EXCLUDED.owner_kind, "
      "role = EXCLUDED.role, added_by = EXCLUDED.added_by",
      {record.package_id, record.owner_id, record.owner_kind, record.role, record.added_by, record.added_at});
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

bool PostgresStore::RemoveRegistryPackageOwner(const std::string &package_id, const std::string &owner_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "DELETE FROM platform_package_owners WHERE package_id = $1 AND owner_id = $2 RETURNING owner_id",
      {package_id, owner_id});
  return PQntuples(result.get()) > 0;
#else
  (void) package_id;
  (void) owner_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

bool PostgresStore::HasRegistryPackageOwner(const std::string &package_id, const std::string &owner_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "SELECT owner_id FROM platform_package_owners WHERE package_id = $1 AND owner_id = $2",
      {package_id, owner_id});
  return PQntuples(result.get()) > 0;
#else
  (void) package_id;
  (void) owner_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryPackageOwners(const std::string &package_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT package_id, owner_id, owner_kind, role, added_by, ") +
          UtcTimestampSql("added_at") + " AS added_at_text FROM platform_package_owners "
          "WHERE package_id = $1 ORDER BY owner_id ASC",
      {package_id});
  nlohmann::json owners = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    owners.push_back(SerializeRegistryPackageOwnerRecord(LoadRegistryPackageOwnerRow(result.get(), row)));
  }
  return owners;
#else
  (void) package_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::string PostgresStore::NextRegistryTokenId() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "SELECT nextval('platform_publish_token_id_seq')::text AS token_sequence",
      {});
  std::ostringstream out;
  out << "tok-" << std::setw(12) << std::setfill('0') << std::stoull(ColumnText(result.get(), 0, "token_sequence"));
  return out.str();
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::UpsertRegistryPublishToken(const RegistryPublishTokenRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_publish_tokens "
      "(token_id, token_hash, owner_id, scopes, package_patterns, expires_at, revoked_at, created_at) "
      "VALUES ($1, $2, $3, $4::jsonb, $5::jsonb, NULLIF($6, '')::timestamptz, NULLIF($7, '')::timestamptz, NULLIF($8, '')::timestamptz) "
      "ON CONFLICT (token_id) DO UPDATE SET token_hash = EXCLUDED.token_hash, owner_id = EXCLUDED.owner_id, "
      "scopes = EXCLUDED.scopes, package_patterns = EXCLUDED.package_patterns, expires_at = EXCLUDED.expires_at, "
      "revoked_at = EXCLUDED.revoked_at",
      {
          record.token_id,
          record.token_hash,
          record.owner_id,
          JsonText(record.scopes),
          JsonText(record.package_patterns),
          record.expires_at,
          record.revoked_at,
          record.created_at,
      });
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<RegistryPublishTokenRecord> PostgresStore::GetRegistryPublishToken(const std::string &token_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(RegistryTokenSelectSql("WHERE token_id = $1"), {token_id});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return LoadRegistryPublishTokenRow(result.get(), 0);
#else
  (void) token_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<RegistryPublishTokenRecord> PostgresStore::FindRegistryPublishTokenByHash(const std::string &token_hash) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(RegistryTokenSelectSql("WHERE token_hash = $1"), {token_hash});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return LoadRegistryPublishTokenRow(result.get(), 0);
#else
  (void) token_hash;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryPublishTokens(const std::string &owner_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = owner_id.empty()
      ? connection.ExecParams(RegistryTokenSelectSql("ORDER BY created_at DESC, token_id ASC"), {})
      : connection.ExecParams(RegistryTokenSelectSql("WHERE owner_id = $1 ORDER BY created_at DESC, token_id ASC"), {owner_id});
  nlohmann::json tokens = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    tokens.push_back(SerializeRegistryPublishTokenRecord(LoadRegistryPublishTokenRow(result.get(), row)));
  }
  return tokens;
#else
  (void) owner_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

bool PostgresStore::RevokeRegistryPublishToken(const std::string &token_id, const std::string &revoked_at) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "UPDATE platform_publish_tokens SET revoked_at = NULLIF($2, '')::timestamptz WHERE token_id = $1 RETURNING token_id",
      {token_id, revoked_at});
  return PQntuples(result.get()) > 0;
#else
  (void) token_id;
  (void) revoked_at;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

int PostgresStore::NextRepositoryVersionSequence(const std::string &repository_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "SELECT COALESCE(MAX(sequence), 0)::text AS max_sequence FROM platform_repository_versions WHERE repository_id = $1",
      {repository_id});
  return std::stoi(ColumnText(result.get(), 0, "max_sequence")) + 1;
#else
  (void) repository_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::UpsertRegistryRepository(const RegistryRepositoryRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_repositories (repository_id, name, tenant_id, policy, created_at) "
      "VALUES ($1, $2, $3, $4::jsonb, NULLIF($5, '')::timestamptz) "
      "ON CONFLICT (repository_id) DO UPDATE SET name = EXCLUDED.name, tenant_id = EXCLUDED.tenant_id, policy = EXCLUDED.policy",
      {record.repository_id, record.name, record.tenant_id, JsonText(record.policy), record.created_at});
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryRepositories() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT repository_id, name, tenant_id, policy::text AS policy_text, ") +
          UtcTimestampSql("created_at") + " AS created_at_text FROM platform_repositories ORDER BY repository_id ASC",
      {});
  nlohmann::json repositories = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    repositories.push_back(SerializeRegistryRepositoryRecord(LoadRegistryRepositoryRow(result.get(), row)));
  }
  return repositories;
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::RecordRegistryRepositoryVersion(const RegistryRepositoryVersionRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_repository_versions "
      "(repository_version_id, repository_id, sequence, change_kind, change_ref, created_at) "
      "VALUES ($1, $2, $3::integer, $4, $5, NULLIF($6, '')::timestamptz) "
      "ON CONFLICT (repository_version_id) DO UPDATE SET change_kind = EXCLUDED.change_kind, change_ref = EXCLUDED.change_ref",
      {
          record.repository_version_id,
          record.repository_id,
          std::to_string(record.sequence),
          record.change_kind,
          record.change_ref,
          record.created_at,
      });
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryRepositoryVersions(const std::string &repository_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT repository_version_id, repository_id, sequence::text AS sequence, change_kind, change_ref, ") +
          UtcTimestampSql("created_at") + " AS created_at_text FROM platform_repository_versions "
          "WHERE repository_id = $1 ORDER BY sequence ASC",
      {repository_id});
  nlohmann::json versions = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    versions.push_back(SerializeRegistryRepositoryVersionRecord(LoadRegistryRepositoryVersionRow(result.get(), row)));
  }
  return versions;
#else
  (void) repository_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::RecordRegistryPublication(const RegistryPublicationRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_publications "
      "(publication_id, repository_version_id, layout_version, root_path, manifest_sha256, tree_size, created_at, verified) "
      "VALUES ($1, $2, $3::integer, $4, $5, $6::integer, NULLIF($7, '')::timestamptz, $8::boolean) "
      "ON CONFLICT (publication_id) DO UPDATE SET root_path = EXCLUDED.root_path, manifest_sha256 = EXCLUDED.manifest_sha256, "
      "tree_size = EXCLUDED.tree_size, verified = EXCLUDED.verified",
      {
          record.publication_id,
          record.repository_version_id,
          std::to_string(record.layout_version),
          record.root_path,
          record.manifest_sha256,
          std::to_string(record.tree_size),
          record.created_at,
          record.verified ? "true" : "false",
      });
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<RegistryPublicationRecord> PostgresStore::GetRegistryPublication(const std::string &publication_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT publication_id, repository_version_id, layout_version::text AS layout_version, root_path, "
                  "manifest_sha256, tree_size::text AS tree_size, ") +
          UtcTimestampSql("created_at") + " AS created_at_text, verified::text AS verified "
          "FROM platform_publications WHERE publication_id = $1",
      {publication_id});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return LoadRegistryPublicationRow(result.get(), 0);
#else
  (void) publication_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryPublications() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT publication_id, repository_version_id, layout_version::text AS layout_version, root_path, "
                  "manifest_sha256, tree_size::text AS tree_size, ") +
          UtcTimestampSql("created_at") + " AS created_at_text, verified::text AS verified "
          "FROM platform_publications ORDER BY created_at ASC, publication_id ASC",
      {});
  nlohmann::json publications = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    publications.push_back(SerializeRegistryPublicationRecord(LoadRegistryPublicationRow(result.get(), row)));
  }
  return publications;
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::UpsertRegistryDistribution(const RegistryDistributionRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_distributions "
      "(distribution_id, repository_id, name, base_url, current_publication_id, previous_publication_id, updated_at) "
      "VALUES ($1, $2, $3, $4, NULLIF($5, ''), NULLIF($6, ''), NULLIF($7, '')::timestamptz) "
      "ON CONFLICT (distribution_id) DO UPDATE SET repository_id = EXCLUDED.repository_id, name = EXCLUDED.name, "
      "base_url = EXCLUDED.base_url, current_publication_id = EXCLUDED.current_publication_id, "
      "previous_publication_id = EXCLUDED.previous_publication_id, updated_at = EXCLUDED.updated_at",
      {
          record.distribution_id,
          record.repository_id,
          record.name,
          record.base_url,
          record.current_publication_id,
          record.previous_publication_id,
          record.updated_at,
      });
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<RegistryDistributionRecord> PostgresStore::GetRegistryDistribution(const std::string &distribution_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT distribution_id, repository_id, name, base_url, "
                  "COALESCE(current_publication_id, '') AS current_publication_id, "
                  "COALESCE(previous_publication_id, '') AS previous_publication_id, ") +
          UtcTimestampSql("updated_at") + " AS updated_at_text "
          "FROM platform_distributions WHERE distribution_id = $1",
      {distribution_id});
  if (PQntuples(result.get()) == 0)
  {
    return std::nullopt;
  }
  return LoadRegistryDistributionRow(result.get(), 0);
#else
  (void) distribution_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryDistributions() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT distribution_id, repository_id, name, base_url, "
                  "COALESCE(current_publication_id, '') AS current_publication_id, "
                  "COALESCE(previous_publication_id, '') AS previous_publication_id, ") +
          UtcTimestampSql("updated_at") + " AS updated_at_text "
          "FROM platform_distributions ORDER BY distribution_id ASC",
      {});
  nlohmann::json distributions = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    distributions.push_back(SerializeRegistryDistributionRecord(LoadRegistryDistributionRow(result.get(), row)));
  }
  return distributions;
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::string PostgresStore::NextRegistryAuditEventId() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "SELECT nextval('platform_registry_audit_event_id_seq')::text AS audit_sequence",
      {});
  std::ostringstream out;
  out << "audit-" << std::setw(12) << std::setfill('0') << std::stoull(ColumnText(result.get(), 0, "audit_sequence"));
  return out.str();
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

void PostgresStore::RecordRegistryAuditEvent(const RegistryAuditEventRecord &record) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_registry_audit_events "
      "(event_id, actor_id, operation, target, request_id, result, created_at) "
      "VALUES ($1, $2, $3, $4::jsonb, $5, $6, NULLIF($7, '')::timestamptz)",
      {
          record.event_id,
          record.actor_id,
          record.operation,
          JsonText(record.target),
          record.request_id,
          record.result,
          record.created_at,
      });
#else
  (void) record;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

nlohmann::json PostgresStore::ListRegistryAuditEvents() const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      std::string("SELECT event_id, actor_id, operation, target::text AS target_text, request_id, result, ") +
          UtcTimestampSql("created_at") + " AS created_at_text "
          "FROM platform_registry_audit_events ORDER BY created_at ASC, event_id ASC",
      {});
  nlohmann::json events = nlohmann::json::array();
  for (int row = 0; row < PQntuples(result.get()); ++row)
  {
    events.push_back(SerializeRegistryAuditEventRecord(LoadRegistryAuditEventRow(result.get(), row)));
  }
  return events;
#else
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

}  // namespace pafio::platform
