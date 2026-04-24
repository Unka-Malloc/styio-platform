# Dependency Usage Boundary

**Purpose:** Record dependency authorization boundaries for `styio-platform`.

**Last updated:** 2026-04-24

`styio-platform` is a GPL-3.0 C++/Python source project. Its current build and test dependency boundary is:

- CMake `FetchContent` downloads `tomlplusplus` for TOML parsing, `nlohmann_json` for JSON serialization, and `googletest` for native test execution.
- Repository Python scripts and tests use the Python standard library only.
- The current service kernel records planned integration boundaries for Boost.Beast/Asio, PostgreSQL, object storage, and regional mirrors as platform capabilities; those integrations must remain explicit service-side dependencies when implemented.
- Runtime registry, control-plane, and transport checks may invoke system tools such as `curl`, `git`, `tar`, `cmake`, and the configured `styio` compiler binary through explicit process boundaries.

Dependency policy:

- No dependency may require commercial authorization, paid licensing, subscription access, membership access, trial-only terms, proprietary-use approval, or private registry access.
- Any future dependency must be listed here with its license evidence, source boundary, and usage boundary before it can pass audit.
- Dependencies used only for tests must stay test-scoped and must not become runtime requirements without this file being updated.
- Generated reports and gate summaries must summarize dependency and license evidence without copying target repository source.
