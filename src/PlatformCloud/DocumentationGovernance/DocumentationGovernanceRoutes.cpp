#include "PlatformCloud/DocumentationGovernance/DocumentationGovernance.hpp"
#include "PlatformCloud/EcosystemManagement/EcosystemManager.hpp"
#include "PlatformService/RouterSupport.hpp"

namespace spio::platform
{

HttpResponse
PlatformRouter::HandleListDocumentationGovernance() const {
  return JsonResponse(
    200,
    SuccessEnvelope("loaded documentation governance", DefaultDocumentationGovernance())
  );
}

HttpResponse
PlatformRouter::HandlePlanDocumentationChange(const HttpRequest &request) const {
  try {
    return JsonResponse(
      200,
      SuccessEnvelope("planned documentation change", BuildDocumentationChangePlan(request.body))
    );
  }
  catch (const std::exception &error) {
    return JsonResponse(
      400,
      FailureEnvelope(
        "documentation change planning failed",
        error.what(),
        "ValidationError",
        "planDocumentationChange",
        2
      )
    );
  }
}

HttpResponse
PlatformRouter::HandleListEcosystemRepositories() const {
  return JsonResponse(
    200,
    SuccessEnvelope("loaded ecosystem repositories", DefaultEcosystemRepositories())
  );
}

HttpResponse
PlatformRouter::HandlePlanEcosystemRelease(const HttpRequest &request) const {
  try {
    return JsonResponse(
      200,
      SuccessEnvelope("planned ecosystem release", BuildEcosystemReleasePlan(request.body))
    );
  }
  catch (const std::exception &error) {
    return JsonResponse(
      400,
      FailureEnvelope(
        "ecosystem release planning failed",
        error.what(),
        "ValidationError",
        "planEcosystemRelease",
        2
      )
    );
  }
}

}  // namespace spio::platform
