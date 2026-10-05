## Changes

<!-- What changed, and why -->

## Verification

<!-- How you verified it. Check what applies -->

- [ ] `npm run compile && npm run lint && npm run test:unit`
- [ ] `python3 tests/test_encodings.py` (if you changed `transcoder/`)
- [ ] `npm run test:integration` (if it affects the extension's integration tests)
- [ ] `python3 tools/gen-tables/gen_tables.py --check` (if you changed a conversion
      table. Don't forget to update the golden hash
      (`tools/gen-tables/golden/tables.sha256`) too)
- [ ] Checked for regressions in the musl static build (if you changed
      `transcoder/`. There are past cases where something worked in the
      native build but broke in the static build)

## Related Issue

<!-- e.g. Closes #xxx -->
