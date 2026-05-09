#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace spio::platform
{

class PlatformRateLimiter
{
public:
  bool Allow(const std::string &subject, const std::string &operation, int limit, int window_seconds, int64_t now_seconds);
  nlohmann::json Snapshot() const;

private:
  struct Bucket
  {
    int64_t window_start = 0;
    int count = 0;
  };

  std::map<std::string, Bucket> buckets_;
};

class PlatformRequestMetrics
{
public:
  void Record(const std::string &operation, int status_code);
  nlohmann::json Snapshot() const;

private:
  int64_t total_requests_ = 0;
  std::map<std::string, int64_t> by_operation_;
  std::map<int, int64_t> by_status_;
};

}  // namespace spio::platform

