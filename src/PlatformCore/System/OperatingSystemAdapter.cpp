#include "PlatformCore/System/OperatingSystemAdapter.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace spio::platform
{

namespace
{

class PosixOperatingSystemAdapter final : public OperatingSystemAdapter
{
public:
  std::optional<std::string> GetEnv(std::string_view name) const override
  {
    const std::string key(name);
    const char *value = std::getenv(key.c_str());
    if (value == nullptr)
    {
      return std::nullopt;
    }
    return std::string(value);
  }

  void CreateDirectories(const fs::path &path) const override
  {
    fs::create_directories(path);
  }

  void RemoveAll(const fs::path &path) const override
  {
    fs::remove_all(path);
  }

  void WriteTextFile(const fs::path &path, std::string_view text) const override
  {
    if (!path.parent_path().empty())
    {
      fs::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
      throw std::runtime_error("failed to write file: " + path.string());
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  spio::ProcessResult RunProcess(const spio::ProcessRequest &request) const override
  {
    return spio::RunProcessChecked(request);
  }

  std::string SendTcpRequest(const TcpRequest &request) const override
  {
    const int fd = ConnectTcp(request);
    try
    {
      SendAll(fd, request.payload);
      const std::string response = ReadAll(fd);
      close(fd);
      return response;
    }
    catch (...)
    {
      close(fd);
      throw;
    }
  }

  void SleepFor(std::chrono::milliseconds duration) const override
  {
    std::this_thread::sleep_for(duration);
  }

private:
  static int ConnectTcp(const TcpRequest &request)
  {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const int rc = getaddrinfo(request.host.c_str(), request.port.c_str(), &hints, &addresses);
    if (rc != 0)
    {
      throw std::runtime_error("failed resolving " + request.context + ": " + gai_strerror(rc));
    }

    int fd = -1;
    for (addrinfo *candidate = addresses; candidate != nullptr; candidate = candidate->ai_next)
    {
      fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
      if (fd < 0)
      {
        continue;
      }
      if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
      {
        break;
      }
      close(fd);
      fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
    {
      throw std::runtime_error("failed connecting for " + request.context);
    }
    return fd;
  }

  static void SendAll(int fd, std::string_view payload)
  {
    size_t written = 0;
    while (written < payload.size())
    {
      const ssize_t rc = ::send(fd, payload.data() + written, payload.size() - written, 0);
      if (rc <= 0)
      {
        throw std::runtime_error("failed writing TCP request");
      }
      written += static_cast<size_t>(rc);
    }
  }

  static std::string ReadAll(int fd)
  {
    std::string out;
    std::array<char, 4096> buffer{};
    while (true)
    {
      const ssize_t rc = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (rc == 0)
      {
        break;
      }
      if (rc < 0)
      {
        throw std::runtime_error("failed reading TCP response");
      }
      out.append(buffer.data(), static_cast<size_t>(rc));
    }
    return out;
  }
};

}  // namespace

const OperatingSystemAdapter &DefaultOperatingSystemAdapter()
{
  static const PosixOperatingSystemAdapter adapter;
  return adapter;
}

}  // namespace spio::platform
