# PlatformSecurity

`PlatformSecurity` owns platform security capabilities and is split by security
responsibility instead of by caller.

- `PlatformCA/` owns certificate authority, trust-anchor, and certificate
  lifecycle code, including local CA initialization and mTLS leaf certificate
  issuance for platform service identities.
- `PlatformClientAuth/` owns client and service identity authentication:
  mTLS identity parsing, platform role recognition, internal role checks, and
  operation-level authorization policy.
- `SecurityHardening/` owns hardening policy and security extension points that
  are not direct client authentication, including registry read/write security
  policy hooks.
