# Audit Specs

**Purpose:** Define reusable audit checklist requirements for platform service and docs changes.

**Last updated:** 2026-04-26

## Scope

Use this collection for durable audit procedures, not one-off investigation
notes.

The auditable-code framework is external and centralized in the sibling
`styio-audit` repository. This repository does not expose an audit interface;
external auditors run `styio-audit` against the worktree and load
`modules/default` plus `for-styio-platform`.
