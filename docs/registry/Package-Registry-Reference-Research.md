# Package Registry Reference Research

**Purpose:** Evaluate crates.io, Pulp, and pacman as reference systems for the Styio package registry architecture.

**Last updated:** 2026-05-09

## Executive Summary

`styio-platform` should not copy one existing repository system end to end. The
best fit is a hybrid:

- Learn product and ecosystem semantics from crates.io: package identity,
  immutable versions, owners/tokens, publish/yank workflow, and a client-facing
  index/artifact split.
- Learn backend repository management from Pulp: versioned repository state,
  publications, distributions, sync/upload, promotion, rollback, and a clear
  management API separated from the content app.
- Learn static read-plane layout from pacman: a small generated repository
  database, package files, signatures, mirrors, and simple HTTP/file hosting.

The resulting Styio model is:

```text
ControlPlane -> PublicationBuilder -> StaticReadPlane -> MirrorSync
```

The write side can be complex and strongly governed. The read side should be
boring, cacheable, mirrorable, and locally verifiable.

## Reference Systems

| System | Best Reference Area | Why It Matters For Styio |
|---|---|---|
| crates.io | Language package registry business model | Closest to a programming language ecosystem with package versions, owners, publish tokens, dependency metadata, and client resolution. |
| Pulp | Repository management backend | Mature separation of repository content, repository versions, publications, distributions, and lifecycle promotion. |
| pacman | Static distribution surface | Minimal generated metadata plus package files and signatures, suitable for mirrors and CDN-like read paths. |

## crates.io Findings

crates.io is the official Rust package registry and its server implementation is
open source. The registry works with Cargo through a package index plus crate
artifact downloads. Cargo supports alternative registries, and the registry
index is the metadata surface Cargo uses for resolution.

Important properties to learn:

- Package identity is developer-facing and stable.
- Versions are immutable after publish; later lifecycle operations such as yank
  should change installability metadata instead of rewriting the artifact.
- The registry API and package index are different concerns: publishing and
  ownership are write-side operations, while resolution and download are
  read-side operations.
- The client should be able to resolve from compact metadata before downloading
  the artifact.
- Registry compatibility matters: Cargo supports registries configured by URL,
  which is a useful model for Styio mirrors and private registries.

Styio mapping:

| crates.io Concept | Styio Equivalent |
|---|---|
| crate | Styio package |
| crate version | package release |
| owners/team permissions | package owner / tenant / scope |
| publish token | platform client auth scope |
| index entry | `PackageIndex` record |
| `.crate` artifact | source/binary Styio artifact |
| yank | release visibility policy, not artifact deletion |

## Pulp Findings

Pulp is closer to an internal repository platform than a language-specific
registry. It models content management as a lifecycle: content is synced or
uploaded into repositories, repository changes produce versions, versions are
published, and distributions expose content through a content app.

Important properties to learn:

- Repository state is versioned. Each content-set change creates an immutable
  repository version.
- A publication is the generated metadata and artifact layout for a repository
  version.
- A distribution is the externally served pointer to a repository, repository
  version, or publication.
- Promotion and rollback are pointer moves, not rebuilds.
- The management API and content-serving app are separate operational surfaces.

Styio mapping:

| Pulp Concept | Styio Equivalent |
|---|---|
| Repository | package namespace/channel |
| RepositoryVersion | immutable registry snapshot |
| Publication | generated static package index/trust/artifact layout |
| Distribution | read-plane base path, mirror, channel, or environment |
| Content App | `StaticReadPlane` |
| Sync | `MirrorSync` |
| Upload | `ControlPlane.publish` |
| Promotion | distribution pointer update |

## pacman Findings

pacman is useful because the repository read path is deliberately simple.
`repo-add` creates and updates a package database for packages built with
`makepkg` and consumed by `pacman`. Package signatures can be embedded or
served alongside packages, and clients can enforce trusted signing policy.

Important properties to learn:

- The read plane can be just generated metadata plus package files.
- Clients fetch repository metadata first, then package artifacts.
- The repository database can be regenerated and mirrored independently of the
  write-side management system.
- Signature policy belongs at the client trust boundary, so offline validation
  remains possible.
- Static repository contents are easy to serve over HTTP, file paths, object
  storage, or mirrors.

Styio mapping:

| pacman Concept | Styio Equivalent |
|---|---|
| repo database | package index snapshot |
| package file | Styio package artifact |
| `.sig` | release signature / trust metadata |
| mirrorlist | registry mirror descriptor |
| `pacman -Sy` metadata refresh | `spio` registry index refresh |

## Target Styio Architecture

```text
PackageRegistry
  ControlPlane
    publish / verify / descriptor / owner / token / yank / audit

  PublicationBuilder
    validate manifest
    place artifacts
    write package index
    calculate checksums
    sign trust metadata
    create immutable publication snapshot

  StaticReadPlane
    serve config, index, trust metadata, and artifacts
    support GET/HEAD only
    keep content CDN/mirror/object-store friendly
    allow offline checksum and signature validation

  MirrorSync
    copy immutable publications
    track replay cursors
    report freshness
    isolate mirrors from write-side outages
```

## PackageRegistry Shape

The registry is a modular monolith with four explicit package-registry layers:

| Layer | Implementation point |
|---|---|
| `ControlPlane` | `PlatformService/Router.cpp` exposes registry APIs for publish, package/release query, yank/unyank, owners, tokens, repositories, publications, and distributions. |
| `PublicationBuilder` | C++ publish path and Python `package_registry_v2.publisher` both generate immutable `_publications/{publication_id}` snapshots and `_distributions/default/current.json`. |
| `StaticReadPlane` | `scripts/registry-v2-static-read-server.py` remains read-only and can serve both root compatibility layout and `_publications/` paths. |
| `MirrorSync` | `PackageRegistry/MirrorSync` consumes the origin distribution pointer, copies the referenced publication, verifies the snapshot, and only then switches the local current pointer. |

The default repository and distribution are both named `default`. The root
registry layout remains compatible with old registry v2 clients:

```text
config.json
index/
artifacts/
trust/
log/
```

Each successful publish or yank/unyank also creates:

```text
_publications/pub-000001/
  config.json
  publication.json
  index/
  artifacts/
  trust/
  log/

_distributions/default/current.json
```

Important invariants:

- Package versions are rejected on duplicate publish.
- Artifacts are content-addressed by SHA-256 and are not removed by yank.
- Yank/unyank changes package index visibility and creates a new repository
  version/publication.
- Distribution promotion and rollback are pointer moves to existing
  publications.
- Static read-plane files remain usable without the control plane online.

## Control Plane vs Static Read Plane

The relation is publication. The control plane accepts mutable business
operations. The publication builder turns accepted state into immutable static
files. The static read plane serves those files.

```text
publish request
  -> auth / policy / validation
  -> artifact persisted
  -> repository version created
  -> publication generated
  -> distribution pointer updated
  -> static read plane serves new snapshot
```

This creates a useful failure boundary:

| Layer | Mutable | Audience | Outage Effect |
|---|---:|---|---|
| ControlPlane | yes | publishers, CI, operators | new publish/admin work stops |
| PublicationBuilder | yes, internal | registry backend | new snapshots stop |
| StaticReadPlane | no | `spio`, workspaces, mirrors | existing packages keep downloading |
| MirrorSync | pointer/copy state | mirror operators | mirrors may lag but remain readable |

## Decisions For styio-platform

- Keep `PackageRegistry` separate from `DeveloperWorkspace`; workspace code may
  consume packages but must not own registry publication state.
- Keep `ControlPlane` write-heavy and policy-heavy.
- Keep `StaticReadPlane` side-effect free and object-store/CDN friendly.
- Treat `PublicationBuilder` as the only layer allowed to produce static index
  and trust metadata.
- Treat `MirrorSync` as a consumer of published snapshots, not as a second
  publisher.
- Keep compatibility facade imports for current scripts while moving
  implementation code into layered directories.

## Sources

- [crates.io source repository](https://github.com/rust-lang/crates.io)
- [Cargo Book: Registries](https://doc.rust-lang.org/cargo/reference/registries.html)
- [crates.io package index](https://index.crates.io/)
- [Pulp: Publish and Host](https://pulpproject.org/pulp_file/docs/user/guides/publish-host/)
- [Pulp: Lifecycle Promotion Support](https://pulpproject.org/pulpcore/docs/user/learn/lifecycle-promotion/)
- [Pulp: Upload Content](https://pulpproject.org/pulp_file/docs/user/guides/upload/)
- [Arch Linux repo-add manual](https://man.archlinux.org/man/repo-add.8)
- [ArchWiki: pacman package signing](https://wiki.archlinux.org/title/Pacman/Package_signing)
- [Arch Linux pacman manual](https://man.archlinux.org/man/pacman)
