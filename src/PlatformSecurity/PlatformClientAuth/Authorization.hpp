#pragma once

#include "PlatformSecurity/PlatformClientAuth/Identity.hpp"

#include <string_view>

namespace spio::platform
{

bool IsInternalRole(const MtlsIdentity &identity);
bool IsAuthorizedForOperation(std::string_view operation_id, const MtlsIdentity &identity);

}  // namespace spio::platform
