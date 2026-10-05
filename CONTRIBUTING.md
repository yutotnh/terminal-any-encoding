# Contributing

## Development Environment Setup

Recommended: use `.devcontainer/` (it comes with a pinned ICU version, the
musl cross-toolchain, and Node.js already set up).

To set things up manually, you need:

- Node.js (the version in `.nvmrc`; this is the version used for local
  development and CI, not the runtime Node.js inside the Extension Host —
  see [AGENTS.md](AGENTS.md#nodejs-version) for that distinction)
- Python 3 (for the table generation pipeline and test runner)
- ICU (the `uconv` command. Pin the version to match the devcontainer — the
  tables depend on the ICU version, so don't bump it casually)
- glibc's `iconv` command with its KOI8-T module (standard on glibc
  distributions; table generation cross-checks KOI8-T against it)
- musl cross-toolchain (e.g. `x86_64-linux-musl-gcc`, needed for the
  statically linked distribution build)

Then run `npm install`.

## Repository Notes

- **Keep `transcoder/` (the luit fork) minimal against upstream.** Don't
  reformat it, and keep hand-written changes as small as possible
  (`.prettierignore` already excludes it from Prettier).
- **Don't hand-edit the generated conversion tables**
  (`transcoder/src/builtin_ja.c` and similar). Regenerate them with
  `tools/gen-tables/gen_tables.py` (after `npm install`: KOI8-T's table comes
  from iconv-lite, the rest from ICU). To change a table, edit
  `tools/gen-tables/converters.json`, regenerate, and update
  `tools/gen-tables/golden/tables.sha256` as well.
- Design rationale and non-obvious implementation details are recorded in
  [docs/transcoder-design.md](docs/transcoder-design.md) and
  [docs/extension-design.md](docs/extension-design.md). Update them too if
  your change affects a design decision.

## Running Tests

```bash
# TypeScript side (unit tests)
npm run compile
npm run lint
npm run test:unit

# TypeScript side (integration tests against a real VS Code instance).
# The extension requires transcoder/bin/luit at startup, so build it first.
# On Linux without a display (including CI), wrap the run in `xvfb-run -a`.
bash transcoder/scripts/build.sh
mkdir -p transcoder/bin && cp transcoder/src/luit transcoder/bin/luit
npm run test:integration

# transcoder (luit fork) build and table-driven tests
# (tests always target transcoder/src/luit, so changing only the build
#  method turns the same tests into a regression check for the
#  distribution build)
bash transcoder/scripts/build.sh          # native (development)
python3 tests/test_encodings.py

# Regression check against the distribution build (a static musl binary).
# There have been cases where something worked natively but broke in the
# static build, so always verify this too.
bash transcoder/scripts/build.sh --musl
python3 tests/test_encodings.py

# Reproducibility check for the conversion tables
python3 tools/gen-tables/gen_tables.py --check
```

The PR template lists which of these to run for a given change.

## Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/)
(`feat:` / `fix:` / `docs:` / `chore:`, etc). Write commit messages, code
comments, and documentation in English.

## Releasing

1. Update `version` in `package.json` and move the `[Unreleased]` notes in
   `CHANGELOG.md` under that version.
2. Push that to `main` and wait for CI to pass. The release workflow
   doesn't run the integration tests (or the spellcheck), so a tag on a
   commit CI hasn't passed can publish a broken build. This
   push also matters for the README: the screenshot isn't in the VSIX
   (`.vscodeignore`), and vsce points the Marketplace page at
   `images/screenshot.png` on the GitHub repository's default branch.
3. Push a `v<version>` tag. `.github/workflows/release.yml` checks that the
   tag matches `package.json`, runs lint/unit tests and the table check,
   builds the transcoder for every platform (running the tests on the ones a
   runner can execute), packages all 7 VSIXes, and only then publishes them.

Publishing needs, in the `release` environment: `AZURE_CLIENT_ID` and
`AZURE_TENANT_ID` (a Microsoft Entra app with a federated credential for
this repository, added to the Marketplace publisher; used by
`vsce publish --azure-credential`), and `OVSX_PAT` for Open VSX.
