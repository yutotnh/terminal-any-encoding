# Contributing

## Development Environment Setup

Recommended: use `.devcontainer/` (it comes with the musl cross-toolchain
and Node.js already set up).

To set things up manually, you need:

- Node.js (the version in `.nvmrc`; this is the version used for local
  development and CI, not the runtime Node.js inside the Extension Host —
  see [AGENTS.md](AGENTS.md#nodejs-version) for that distinction)
- Python 3 (for the table generation pipeline and test runner)
- For the statically linked distribution build (`build.sh --musl`), either
  Docker (build in an Alpine container, as CI does) or a musl
  cross-toolchain (e.g. `x86_64-linux-musl-gcc`, set up in the devcontainer)

Then run `npm install`.

## Repository Notes

- **Keep `transcoder/` (the luit fork) minimal against upstream.** Don't
  reformat it, and keep hand-written changes as small as possible
  (`.prettierignore` already excludes it from Prettier).
- **Don't hand-edit the generated conversion tables**
  (`transcoder/src/builtin_ja.c` and similar). Regenerate them with
  `tools/gen-tables/gen_tables.py` (after `npm install`: the tables come from
  iconv-lite, the library VS Code decodes files with). To change a table, edit
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
# Every byte sequence and character of every encoding against VS Code's
# editor (iconv-lite; needs npm install). In parallel, about 10 seconds.
python3 tests/test_editor_parity.py

# Regression check against the distribution build (a static musl binary).
# There have been cases where something worked natively but broke in the
# static build, so always verify this too.
bash transcoder/scripts/build.sh --musl
python3 tests/test_encodings.py
python3 tests/test_editor_parity.py

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
   `CHANGELOG.md` under a `## [<version>] - <date>` heading. Those notes
   become the GitHub release's notes, and the release workflow stops before
   building anything if they're missing.
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
   Once both registries have the version, it creates the GitHub release with
   the VSIXes attached. Releases are immutable in this repository, so check
   the notes before tagging. If only that last step fails, don't rerun the
   job (the registries would reject the same version again); create the
   release by hand with the same `gh release create` command instead,
   using the VSIXes from the run's `vsix-*` artifacts.

Publishing needs three secrets in the `release` environment, which only
accepts `v*` tags:

- `AZURE_CLIENT_ID` and `AZURE_TENANT_ID`: the client ID of a user-assigned
  managed identity in Azure and its tenant's ID. No Marketplace PAT is
  stored: Azure DevOps retires global PATs on 2026-12-01, and
  `vsce publish --azure-credential` signs in as this identity through
  GitHub OIDC (`azure/login`). The identity needs:
  - a federated credential for GitHub Actions with the subject
    `repo:yutotnh@57719497/terminal-any-encoding@1405905716:environment:release`.
    The repository uses GitHub's immutable subject format (owner and
    repository IDs after `@`), so the name-only subject
    `repo:yutotnh/terminal-any-encoding:...` never matches.
  - membership (Contributor) in the Marketplace publisher. The ID to add
    isn't anything the Azure portal shows: it's the `id` that
    `az rest -u https://app.vssps.visualstudio.com/_apis/profile/profiles/me --resource 499b84ac-1321-427f-aa17-267ca6975798`
    returns when signed in as the identity, so get it from a workflow job
    that runs `azure/login` in the `release` environment.
- `OVSX_PAT`: an Open VSX token from an account that has signed the Eclipse
  Publisher Agreement. Open VSX has no OIDC sign-in.
