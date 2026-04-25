# styio-platform Contracts

This directory owns service-side and platform-side contract packages migrated
from `styio-spio`.

- `compile-plan/` is the compiler handoff format consumed by platform workers.
- `hosted-control-plane/` is the native JSON hosted workspace contract package.
- `platform-control-plane/` is the native JSON service-kernel contract package
  for the first runnable cloud control plane.
- `registry-control-plane/` is the native JSON server-side registry
  write/control contract package.
- `registry-v2/` is retained here for server validation and compatibility with
  `styio-spio` package-manager clients.

Contract packages are maintained as repo-native JSON contracts and examples.
Markdown describes ownership and stability rules; executable gates validate the
JSON packages directly.
