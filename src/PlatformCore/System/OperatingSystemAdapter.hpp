#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "PlatformCore/Core/Process.hpp"

namespace pafio::platform
{

// Raw TCP request used by workers and test adapters. Callers provide a complete
// protocol payload so the adapter can stay transport-only.
struct TcpRequest
{
  std::string host;
  std::string port;
  std::string payload;
  std::string context = "tcp request";
};

// Centralizes operating-system effects behind a narrow interface so workspace,
// source-fetch, and worker code can be tested without shelling out directly.
class OperatingSystemAdapter
{
public:
  virtual ~OperatingSystemAdapter() = default;

  virtual std::optional<std::string> GetEnv(std::string_view name) const = 0;
  virtual void CreateDirectories(const std::filesystem::path &path) const = 0;
  virtual void RemoveAll(const std::filesystem::path &path) const = 0;
  virtual void WriteTextFile(const std::filesystem::path &path, std::string_view text) const = 0;
  virtual pafio::ProcessResult RunProcess(const pafio::ProcessRequest &request) const = 0;
  virtual std::string SendTcpRequest(const TcpRequest &request) const = 0;
  virtual void SleepFor(std::chrono::milliseconds duration) const = 0;
};

const OperatingSystemAdapter &DefaultOperatingSystemAdapter();

}  // namespace pafio::platform
