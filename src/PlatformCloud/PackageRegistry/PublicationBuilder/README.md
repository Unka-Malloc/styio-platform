# Package Registry Publication Builder

Builds immutable registry publications from accepted package input. This layer
owns registry v2 metadata generation, artifact placement, checksum calculation,
signing material usage, and publication snapshot layout.

The `package_registry_v2` package here contains the active publish/keygen
implementation. The top-level `PackageRegistry/package_registry_v2` package is
a compatibility facade used by existing scripts and tests.
