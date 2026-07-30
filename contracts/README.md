# styio-platform Contracts

This directory owns Styio Platform service contracts.

- `hosted-control-plane/` is the native JSON hosted workspace contract package.
- `platform-control-plane/` is the native JSON service-kernel contract package
  for the first runnable cloud control plane.
- `registry-control-plane/` is the native JSON server-side registry
  write/control contract package.
- `registry-v2/` is retained here for server validation and compatibility with
  `pafio-nightly` package-manager clients.

Contract packages are maintained as repo-native JSON contracts and examples.
Markdown describes ownership and stability rules; executable gates validate the
JSON packages directly.

`styio-platform` owns hosted workspace, registry, job, worker, and mirror
contracts. Pafio owns project metadata and workflow contracts; Styio owns
compiler-facing contracts.
