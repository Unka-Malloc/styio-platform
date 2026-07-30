#include "PlatformCore/Core/Paths.hpp"

namespace fs = std::filesystem;

namespace pafio
{

fs::path ProjectRoot()
{
  return fs::path(PAFIO_PROJECT_ROOT);
}

fs::path CanonicalAbsolutePath(const fs::path &path)
{
  return fs::absolute(path).lexically_normal();
}

}  // namespace pafio
