# Package Registry Publication Builder

Builds immutable registry publications from accepted package input. This layer
owns registry v2 metadata generation, artifact placement, checksum calculation,
signing material usage, and publication snapshot layout.

The `package_registry_v2` package here is the only publish/keygen
implementation. Shared cryptographic and serialization primitives live in
`PackageRegistry/package_registry_v2/common.py`; callers import publication
operations from this module directly.
