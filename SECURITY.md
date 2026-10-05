# Security Policy

## About This Extension

This extension bundles a native binary (`transcoder/`, a fork of
[luit](https://invisible-island.net/luit/)) as a prebuilt binary and runs it
as the integrated terminal's `shellPath`. This gives it a larger attack
surface than a typical TypeScript-only extension, so security issues are
treated with particular care.

- The provenance of the bundled binary's build can be verified
  ([`actions/attest-build-provenance`](https://github.com/actions/attest-build-provenance)).
- `transcoder/vendor/luit-upstream/` is an unmodified copy of upstream, so
  the fork's changes are the diff against `transcoder/src/`. The reasons
  for them are in [docs/transcoder-design.md](docs/transcoder-design.md).

## Reporting a Vulnerability

If you find a vulnerability, please **do not open a public issue**. Report it
through GitHub [Security Advisories](../../security/advisories/new) instead,
so the reporting, fix, and disclosure timing can be coordinated privately.

Examples of what's in scope:

- Vulnerabilities in the bundled `transcoder` binary (e.g. buffer overflows —
  it processes external input via the PTY, so this needs particular care)
- Command injection or similar issues from the extension mishandling user
  input or host-side paths
- Known vulnerabilities in npm dependencies that Dependabot doesn't catch

## Supported Versions

Only the latest release is supported. There are no backports to older
versions.
