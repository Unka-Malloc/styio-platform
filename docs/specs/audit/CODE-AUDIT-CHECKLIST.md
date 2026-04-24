# Code Audit Checklist

**Purpose:** Provide the minimum review checklist for platform code, contract, and server-script changes.

**Last updated:** 2026-04-24

## Checklist

- Verify the changed service boundary is documented in governance or operations docs.
- Run the relevant native, Python, and contract gates.
- Confirm generated contract artifacts are in sync when `contracts/` changes.
- Update the owning team runbook and `docs/teams/DOC-STATS.md` when ownership surfaces change.
