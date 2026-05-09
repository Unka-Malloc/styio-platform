# PlatformCA

This directory owns platform certificate authority and trust-anchor code.

`CertificateAuthority.*` currently provides local CA initialization and mTLS
leaf certificate issuance for platform service identities. Production
deployments may still mount externally managed certificates, but service code
must consume certificate lifecycle helpers from here instead of creating CA
logic in `PlatformService` or deployment scripts.
