#pragma once

#include "PlatformCore/Core/Process.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace spio::platform
{

struct TcpRequest
{
  std::string host;
  std::string port;
  std::string payload;
  std::string context = "tcp request";
};

class OperatingSystemAdapter
{
public:
  virtual ~OperatingSystemAdapter() = default;

  virtual std::optional<std::string> GetEnv(std::string_view name) const = 0;
  virtual void CreateDirectories(const std::filesystem::path &path) const = 0;
  virtual void RemoveAll(const std::filesystem::path &path) const = 0;
  virtual void WriteTextFile(const std::filesystem::path &path, std::string_view text) const = 0;
  virtual spio::ProcessResult RunProcess(const spio::ProcessRequest &request) const = 0;
  virtual std::string SendTcpRequest(const TcpRequest &request) const = 0;
  virtual void SleepFor(std::chrono::milliseconds duration) const = 0;
};

const OperatingSystemAdapter &DefaultOperatingSystemAdapter();

}  // namespace spio::platform
