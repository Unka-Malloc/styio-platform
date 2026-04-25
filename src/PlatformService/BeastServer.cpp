#include "PlatformService/BeastServer.hpp"

#if __has_include(<boost/asio.hpp>) && __has_include(<boost/beast.hpp>)
#define STYIO_PLATFORM_HAS_BOOST_BEAST 1
#else
#define STYIO_PLATFORM_HAS_BOOST_BEAST 0
#endif

namespace spio::platform
{

nlohmann::json DescribeBeastServerCapability()
{
  return {
      {"target", "Boost.Beast/Asio"},
      {"headers_available", STYIO_PLATFORM_HAS_BOOST_BEAST == 1},
      {"role", "production HTTP adapter for PlatformRouter"},
      {"fallback", "PlatformRouter remains directly testable without network dependencies"},
  };
}

}  // namespace spio::platform
