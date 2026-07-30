#pragma once

#include "PlatformSecurity/PlatformClientAuth/Identity.hpp"

#include <string_view>

namespace pafio::platform
{

bool IsInternalRole(const MtlsIdentity &identity);
bool IsAuthorizedForOperation(std::string_view operation_id, const MtlsIdentity &identity);

}  // namespace pafio::platform
