## Summary

-

## Validation

- [ ] `cmake --build build-codex -j 4`
- [ ] `ctest --test-dir build-codex --output-on-failure`
- [ ] `bash scripts/docs-gate.sh`
- [ ] `python3 scripts/repo-hygiene-gate.py --mode working`
- [ ] `git diff --check`

## Notes

-
