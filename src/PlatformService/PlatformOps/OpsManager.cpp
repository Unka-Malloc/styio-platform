#include "PlatformService/PlatformOps/OpsManager.hpp"

namespace pafio::platform
{

bool PlatformRateLimiter::Allow(
    const std::string &subject,
    const std::string &operation,
    const int limit,
    const int window_seconds,
    const int64_t now_seconds)
{
  if (limit <= 0 || window_seconds <= 0)
  {
    return true;
  }
  Bucket &bucket = buckets_[subject + ":" + operation];
  if (bucket.window_start == 0 || now_seconds - bucket.window_start >= window_seconds)
  {
    bucket.window_start = now_seconds;
    bucket.count = 0;
  }
  ++bucket.count;
  return bucket.count <= limit;
}

nlohmann::json PlatformRateLimiter::Snapshot() const
{
  nlohmann::json buckets = nlohmann::json::object();
  for (const auto &[key, bucket] : buckets_)
  {
    buckets[key] = {
        {"window_start", bucket.window_start},
        {"count", bucket.count},
    };
  }
  return {{"buckets", buckets}};
}

void PlatformRequestMetrics::Record(const std::string &operation, const int status_code)
{
  ++total_requests_;
  ++by_operation_[operation];
  ++by_status_[status_code];
}

nlohmann::json PlatformRequestMetrics::Snapshot() const
{
  nlohmann::json operations = nlohmann::json::object();
  for (const auto &[operation, count] : by_operation_)
  {
    operations[operation] = count;
  }
  nlohmann::json statuses = nlohmann::json::object();
  for (const auto &[status, count] : by_status_)
  {
    statuses[std::to_string(status)] = count;
  }
  return {
      {"total_requests", total_requests_},
      {"by_operation", operations},
      {"by_status", statuses},
  };
}

}  // namespace pafio::platform

