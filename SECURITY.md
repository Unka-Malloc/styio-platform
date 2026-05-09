# Security Policy

## Supported Versions

`styio-platform` is currently published as a developer-preview project. Security
fixes are accepted on the active development branch and are expected to be
included in the next tagged release once release branches are introduced.

## Reporting a Vulnerability

Do not open a public issue for a suspected vulnerability. Report security issues
privately to the project maintainers through the repository owner's preferred
security contact.

Please include:

- affected commit, tag, or branch
- impacted component, endpoint, script, or deployment path
- reproduction steps or proof-of-concept details
- expected impact and any known mitigations

The maintainers will acknowledge valid reports, coordinate a fix, and publish
public details only after users have had a reasonable opportunity to update.

## Security Scope

In scope:

- registry control-plane authorization and token handling
- publication, mirror, and static read-plane integrity checks
- mTLS identity parsing and certificate management
- Postgres and object-store persistence boundaries
- deployment scripts and Helm defaults that could expose credentials or write
  surfaces

Out of scope:

- vulnerabilities in local developer machines or third-party infrastructure
- unsupported forks or modified deployments
- denial-of-service reports without a concrete security impact

## Release Checks

Before publishing source or binary releases, run:

```sh
python3 scripts/repo-hygiene-gate.py --mode working
python3 scripts/repo-hygiene-gate.py --mode secrets-history
git diff --check
```
