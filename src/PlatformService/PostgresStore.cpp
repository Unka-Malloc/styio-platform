#include "PlatformService/PostgresStore.hpp"

namespace spio::platform
{

bool LooksLikePostgresDsn(std::string_view value)
{
  return value.starts_with("postgres://") || value.starts_with("postgresql://") || value.starts_with("host=");
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

}  // namespace spio::platform
