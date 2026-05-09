#include "PlatformCore/Core/Paths.hpp"

namespace fs = std::filesystem;

namespace spio
{

fs::path ProjectRoot()
{
  return fs::path(SPIO_PROJECT_ROOT);
}

fs::path CanonicalAbsolutePath(const fs::path &path)
{
  return fs::absolute(path).lexically_normal();
}

}  // namespace spio
