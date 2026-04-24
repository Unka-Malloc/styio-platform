# Post Commit CI Checks

**Purpose:** Require post-push validation for `styio-platform` changes before work is reported as closed.

**Last updated:** 2026-04-24

## Workflow

After pushing `ai-dev`, inspect the real GitHub Actions runs for the pushed
commit and keep the turn open while failures are actionable. Local gates are a
pre-push floor; they do not replace post-push Actions monitoring.

## Local Floor

Run the platform build, native tests, Python unit tests, docs audit, and repo
hygiene gate before publishing a branch update.
