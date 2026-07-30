#include "PlatformSecurity/SecurityHardening/RegistrySecurity.hpp"

#include "PlatformCore/Core/Errors.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

enum class SecurityHandlerResult
{
  kContinue,
  kResolved,
};

std::string NormalizeRegistryRoot(std::string value)
{
  while (!value.empty() && value.back() == '/')
  {
    value.pop_back();
  }
  return value;
}

bool IsHttpRegistryRoot(const std::string &value)
{
  return value.starts_with("http://") || value.starts_with("https://");
}

bool IsFileRegistryRoot(const std::string &value)
{
  return value.starts_with("file://");
}

using ReadSecurityHandler =
    SecurityHandlerResult (*)(const pafio::RegistryReadSecurityRequest &request, pafio::RegistryReadSecurityDecision &decision);
using WriteSecurityHandler =
    SecurityHandlerResult (*)(const pafio::RegistryWriteSecurityRequest &request, pafio::RegistryWriteSecurityDecision &decision);

template <typename Request, typename Decision, typename Handler, size_t N>
Decision RunSecurityChain(
    const Request &request,
    Decision decision,
    const std::array<Handler, N> &handlers,
    const char *chain_name)
{
  for (const Handler handler : handlers)
  {
    if (handler(request, decision) == SecurityHandlerResult::kResolved)
    {
      return decision;
    }
  }

  throw std::logic_error(std::string(chain_name) + " registry security chain did not resolve");
}

SecurityHandlerResult NormalizeReadRegistryRoot(
    const pafio::RegistryReadSecurityRequest &request,
    pafio::RegistryReadSecurityDecision &decision)
{
  decision.registry_root = NormalizeRegistryRoot(request.registry_root);
  return SecurityHandlerResult::kContinue;
}

SecurityHandlerResult ValidateReadRegistryRootScheme(
    const pafio::RegistryReadSecurityRequest &request,
    pafio::RegistryReadSecurityDecision &decision)
{
  if (!IsFileRegistryRoot(decision.registry_root) && !IsHttpRegistryRoot(decision.registry_root))
  {
    throw pafio::FetchError("registry root must use file://, http://, or https://: " + request.registry_root);
  }
  return SecurityHandlerResult::kContinue;
}

SecurityHandlerResult ResolvePublicDefaultReadAccess(
    const pafio::RegistryReadSecurityRequest &request,
    pafio::RegistryReadSecurityDecision &decision)
{
  (void) request;
  decision.request_headers.clear();
  decision.provider_name = "public-default";
  return SecurityHandlerResult::kResolved;
}

SecurityHandlerResult NormalizeWriteRegistryRoot(
    const pafio::RegistryWriteSecurityRequest &request,
    pafio::RegistryWriteSecurityDecision &decision)
{
  decision.registry_root = NormalizeRegistryRoot(request.registry_root);
  return SecurityHandlerResult::kContinue;
}

SecurityHandlerResult ValidateWriteRegistryRootScheme(
    const pafio::RegistryWriteSecurityRequest &request,
    pafio::RegistryWriteSecurityDecision &decision)
{
  if (!IsHttpRegistryRoot(decision.registry_root))
  {
    throw pafio::PublishError("remote registry publish requires an http:// or https:// registry root: " + request.registry_root);
  }
  return SecurityHandlerResult::kContinue;
}

SecurityHandlerResult RejectOpenSourceWriteSecurityHooks(
    const pafio::RegistryWriteSecurityRequest &request,
    pafio::RegistryWriteSecurityDecision &decision)
{
  (void) decision;
  if (request.profile_name.has_value() || request.policy_file.has_value() || !request.explicit_request_headers.empty())
  {
    throw pafio::PublishError(
        "registry write security hooks require a private module under src-private/PlatformSecurity and are not available "
        "in the open-source core");
  }
  return SecurityHandlerResult::kContinue;
}

SecurityHandlerResult ResolvePublicDefaultWriteAccess(
    const pafio::RegistryWriteSecurityRequest &request,
    pafio::RegistryWriteSecurityDecision &decision)
{
  (void) request;
  decision.request_headers.clear();
  decision.provider_name = "public-default";
  decision.mode = "anonymous";
  decision.profile_name = std::nullopt;
  return SecurityHandlerResult::kResolved;
}

const std::array<ReadSecurityHandler, 3> kDefaultReadSecurityHandlers = {
    NormalizeReadRegistryRoot,
    ValidateReadRegistryRootScheme,
    ResolvePublicDefaultReadAccess,
};

const std::array<WriteSecurityHandler, 4> kDefaultWriteSecurityHandlers = {
    NormalizeWriteRegistryRoot,
    ValidateWriteRegistryRootScheme,
    RejectOpenSourceWriteSecurityHooks,
    ResolvePublicDefaultWriteAccess,
};

pafio::RegistryReadSecurityResolver &ReadSecurityResolverSlot()
{
  static pafio::RegistryReadSecurityResolver resolver = pafio::ResolveDefaultRegistryReadSecurity;
  return resolver;
}

pafio::RegistryWriteSecurityResolver &WriteSecurityResolverSlot()
{
  static pafio::RegistryWriteSecurityResolver resolver = pafio::ResolveDefaultRegistryWriteSecurity;
  return resolver;
}

}  // namespace

namespace pafio
{

RegistryReadSecurityDecision ResolveDefaultRegistryReadSecurity(const RegistryReadSecurityRequest &request)
{
  return RunSecurityChain(
      request,
      RegistryReadSecurityDecision{},
      kDefaultReadSecurityHandlers,
      "default read");
}

RegistryWriteSecurityDecision ResolveDefaultRegistryWriteSecurity(const RegistryWriteSecurityRequest &request)
{
  return RunSecurityChain(
      request,
      RegistryWriteSecurityDecision{},
      kDefaultWriteSecurityHandlers,
      "default write");
}

RegistryReadSecurityResolver RegisterRegistryReadSecurityResolver(RegistryReadSecurityResolver resolver)
{
  RegistryReadSecurityResolver &slot = ReadSecurityResolverSlot();
  RegistryReadSecurityResolver previous = slot;
  slot = resolver != nullptr ? resolver : ResolveDefaultRegistryReadSecurity;
  return previous;
}

RegistryWriteSecurityResolver RegisterRegistryWriteSecurityResolver(RegistryWriteSecurityResolver resolver)
{
  RegistryWriteSecurityResolver &slot = WriteSecurityResolverSlot();
  RegistryWriteSecurityResolver previous = slot;
  slot = resolver != nullptr ? resolver : ResolveDefaultRegistryWriteSecurity;
  return previous;
}

RegistryReadSecurityDecision ResolveRegistryReadSecurity(const RegistryReadSecurityRequest &request)
{
  return ReadSecurityResolverSlot()(request);
}

RegistryWriteSecurityDecision ResolveRegistryWriteSecurity(const RegistryWriteSecurityRequest &request)
{
  return WriteSecurityResolverSlot()(request);
}

}  // namespace pafio
