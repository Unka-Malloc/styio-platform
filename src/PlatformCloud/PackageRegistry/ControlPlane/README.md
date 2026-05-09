# Package Registry Control Plane

Owns write-side registry operations: publish, verify, descriptor, authorization
surface, audit, package/release queries, package owner management, publish
tokens, yank/unyank, repositories, publications, and distribution promotion.

The registry route table lives in `RegistryRoutes.cpp`. Handler dispatch still
uses `PlatformService/Router.cpp` as the embedding adapter while the registry
control-plane API surface is declared in `manifests/package-registry.yaml`.
