# Spio Registry Client

Owns registry consumption and snapshot extraction for `file://`, `http://`, and
`https://` sources. Shared cache root layout belongs to
`src/PlatformStorage/PlatformCache/`.

Do not place server upload or publish transport code here.
Do not place private trust allowlists, read credentials, or environment-specific registry policy here; those belong behind `src/PlatformSecurity/SecurityHardening/` and optional `src-private/`.
