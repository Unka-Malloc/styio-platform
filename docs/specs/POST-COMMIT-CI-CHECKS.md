# Post Commit CI Checks

**Purpose:** Require post-push validation for `styio-platform` changes before work is reported as closed.

**Last updated:** 2026-04-25

## Workflow

After pushing `ai-dev`, inspect the real remote status checks for the pushed
commit and keep the turn open while failures are actionable. Local gates are a
pre-push floor; they do not replace post-push status monitoring.

The open CI/CD source of truth is the Tekton pipeline in
`deploy/tekton/styio-platform-ci/`. GitHub Actions workflows are compatibility
wrappers for GitHub-hosted required checks and must delegate to repository
scripts rather than owning unique CI logic.

## Local Floor

Run the platform build, native tests, Python unit tests, docs audit, and repo
hygiene gate before publishing a branch update.

## Delivery Ruleset Governance

Required GitHub merge gates are maintained through GitHub Rulesets, not legacy
classic branch protection. `ai-dev` and protected release/default branches must
have an active Ruleset requiring the `audit` status check from the `styio-audit`
workflow, with strict required status checks enabled.

Gate audits must inspect effective branch rules, for example:

```bash
gh api repos/eBioRing/styio-platform/rules/branches/ai-dev
```

Do not use `branches/ai-dev/protection/required_status_checks` as the authority
for this repository. That legacy classic endpoint can return 404 even when the
Ruleset gate is active.
