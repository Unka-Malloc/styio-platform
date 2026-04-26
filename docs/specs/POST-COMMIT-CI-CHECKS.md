# Post Commit CI Checks

**Purpose:** Require post-push validation for `styio-platform` changes before work is reported as closed.

**Last updated:** 2026-04-26

## Workflow

After pushing `nightly`, inspect the real GitHub Actions runs for the pushed
commit and keep the turn open while failures are actionable. Local gates are a
pre-push floor; they do not replace post-push Actions monitoring.

## Local Floor

Run the platform build, native tests, Python unit tests, docs audit, repo
hygiene gate, and delivery gate before publishing a branch update. The GitHub
Actions floor is `local-ci-gate` plus the repository-local audit checks.
`local-ci-gate` is the platform repository's own CI surface; it is not the
shared Styio ecosystem resource gate modeled by upstream `styio-ci-gate`.
