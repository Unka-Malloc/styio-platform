# Package Registry Static Read Plane

Owns the CDN- and mirror-friendly read surface. The read plane should stay
side-effect free: clients fetch config, trust metadata, package indexes, and
artifacts through GET/HEAD style access and validate checksums/signatures
locally.

The `package_registry_v2` package here contains the static publication
validator. The static HTTP server remains in `scripts/registry-v2-static-read-server.py`.
