import * as assert from "node:assert";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

/**
 * Verifies that the template strings (the literal first argument) of every
 * `vscode.l10n.t(...)` call in `src/extension.ts` match the keys in
 * `l10n/bundle.l10n.ja.json` exactly.
 *
 * Since this repo has only a handful of target strings, it hand-writes
 * `bundle.l10n.ja.json` instead of using an extraction tool like
 * `@vscode/l10n-dev`. Because it's hand-maintained, the source and the
 * bundle can drift apart without a runtime error (an untranslated key just
 * shows the English text as-is), which is easy to miss — so this
 * mechanically cross-checks them, the same way the existing
 * package.json/encodings.ts manual-sync tests do.
 */

/** A simple unescaper that pulls out one `'...'`/`"..."` literal from source
 * (accounting for backslash escapes). This repo's l10n.t() calls only use
 * simple `\'`/`\"` escapes, so this is sufficient. */
function unescapeJsString(raw: string): string {
  return raw.replace(/\\(.)/g, "$1");
}

function extractL10nTemplates(sourceText: string): string[] {
  const pattern = /\bvscode\.l10n\.t\(\s*(['"])((?:\\.|(?!\1).)*)\1/g;
  const templates: string[] = [];
  for (const m of sourceText.matchAll(pattern)) {
    templates.push(unescapeJsString(m[2]));
  }
  return templates;
}

test("every vscode.l10n.t() template in extension.ts matches l10n/bundle.l10n.ja.json exactly", () => {
  const extensionTsPath = path.join(
    __dirname,
    "..",
    "..",
    "src",
    "extension.ts",
  );
  const bundlePath = path.join(
    __dirname,
    "..",
    "..",
    "l10n",
    "bundle.l10n.ja.json",
  );

  const sourceText = fs.readFileSync(extensionTsPath, "utf-8");
  const templates = extractL10nTemplates(sourceText);
  assert.ok(
    templates.length > 0,
    "found no l10n.t() calls (the regex may be broken)",
  );

  const bundle = JSON.parse(fs.readFileSync(bundlePath, "utf-8")) as Record<
    string,
    string
  >;
  const bundleKeys = new Set(Object.keys(bundle));
  const templateSet = new Set(templates);

  const missingFromBundle = templates.filter((t) => !bundleKeys.has(t));
  assert.deepStrictEqual(
    missingFromBundle,
    [],
    `Untranslated templates missing from bundle.l10n.ja.json:\n${missingFromBundle.join("\n---\n")}`,
  );

  const unusedInBundle = [...bundleKeys].filter((k) => !templateSet.has(k));
  assert.deepStrictEqual(
    unusedInBundle,
    [],
    `bundle.l10n.ja.json has keys no longer referenced from extension.ts:\n${unusedInBundle.join("\n---\n")}`,
  );
});
