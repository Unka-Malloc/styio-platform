#include "PlatformService/PostgresStore.hpp"

#include <array>
#include <sstream>
#include <utility>

#if STYIO_PLATFORM_HAS_LIBPQ
#include <libpq-fe.h>
#endif

namespace spio::platform
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
      << "SELECT job_id, tenant_id, workspace_id, action, status, region, worker_pool_key, "
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

CREATE TABLE IF NOT EXISTS platform_jobs (
  job_id TEXT PRIMARY KEY,
  tenant_id TEXT NOT NULL REFERENCES platform_tenants(tenant_id),
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
        "(job_id, tenant_id, workspace_id, action, status, region, worker_pool_key, request, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8::jsonb, now()) "
        "ON CONFLICT (job_id) DO UPDATE SET status = EXCLUDED.status, worker_id = NULL, "
        "request = EXCLUDED.request, updated_at = now(), finished_at = NULL",
        {job.job_id, job.tenant_id, job.workspace_id, job.action, job.status, job.region, job.worker_pool_key, JsonText(job.job_request)});
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
      connection.Exec("ROLLBACK");
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
    const std::string &replay_cursor) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  connection.ExecParams(
      "INSERT INTO platform_mirror_cursors (mirror_id, region, origin, freshness, replay_cursor, updated_at) "
      "VALUES ($1, $2, $3, $4, $5, now()) "
      "ON CONFLICT (mirror_id) DO UPDATE SET region = EXCLUDED.region, origin = EXCLUDED.origin, "
      "freshness = EXCLUDED.freshness, replay_cursor = EXCLUDED.replay_cursor, updated_at = now()",
      {mirror_id, region, origin, freshness, replay_cursor});
#else
  (void) mirror_id;
  (void) region;
  (void) origin;
  (void) freshness;
  (void) replay_cursor;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

std::optional<MirrorCursorRecord> PostgresStore::GetMirrorState(const std::string &mirror_id) const
{
#if STYIO_PLATFORM_HAS_LIBPQ
  PgConnection connection(dsn_);
  PgResult result = connection.ExecParams(
      "SELECT mirror_id, region, origin, freshness, replay_cursor FROM platform_mirror_cursors WHERE mirror_id = $1",
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
  };
#else
  (void) mirror_id;
  throw PostgresStoreError("postgres driver is not available; install libpq development headers");
#endif
}

}  // namespace spio::platform
