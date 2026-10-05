---
name: vscode-integration-tests
description: Gotchas for running/debugging VS Code integration tests (npm run test:integration) — where the prerequisites live and the proposed API flag.
---

# VS Code integration tests (`npm run test:integration`)

- Prerequisites (building `transcoder/bin/luit` first, `xvfb-run -a` on
  Linux) are in the "Running Tests" section of `CONTRIBUTING.md`. If the
  extension fails to activate, check that `transcoder/bin/luit` exists before
  suspecting a VS Code version incompatibility.
- Some tests use `onDidWriteTerminalData` (a proposed API). It's only enabled
  for test runs via `--enable-proposed-api`; product code doesn't use it.
