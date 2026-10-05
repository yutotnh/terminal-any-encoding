import * as assert from "node:assert";
import { test } from "node:test";
import {
  LocaleResolution,
  resolveLocaleEnv,
  suggestLocaleName,
} from "../locale";

/** The LANG the resolution would give the shell, or undefined if none */
function langOf(result: LocaleResolution): string | undefined {
  return result.kind === "matched" ? result.locale : undefined;
}

test("on successful probe: euc-jp-2007 sets ja_JP.EUC-JP when there's a match in availableLocales", () => {
  const base = { LANG: "en_US.UTF-8" };
  const result = resolveLocaleEnv(
    "euc-jp-2007",
    base,
    ["ja_JP.eucjp", "ja_JP.utf8"],
    undefined,
  );
  assert.strictEqual(langOf(result), "ja_JP.EUC-JP");
});

test("regression test for the euc-jp-2007 CODESET_ALIASES override: the luitEncoding string alone doesn't match, so the explicit alias (EUC-JP) is used instead", () => {
  const base = {};
  const result = resolveLocaleEnv(
    "euc-jp-2007",
    base,
    ["ja_JP.eucjp"],
    undefined,
  );
  assert.strictEqual(
    langOf(result),
    "ja_JP.EUC-JP",
    "this would be unset if CODESET_ALIASES['euc-jp-2007'] were missing",
  );
});

test("CP932 reports noMatch when no SJIS-family locale is in availableLocales", () => {
  const base = { LANG: "en_US.UTF-8" };
  const result = resolveLocaleEnv(
    "CP932",
    base,
    ["ja_JP.eucjp", "ja_JP.utf8"],
    "ja",
  );
  assert.deepStrictEqual(result, { kind: "noMatch" });
});

test("CP932 picks up a host-specific locale like ja_JP.sjis when it actually exists (a distro-specific locale)", () => {
  const base = { LANG: "ja_JP.UTF-8" };
  const result = resolveLocaleEnv(
    "CP932",
    base,
    ["ja_JP.sjis", "ja_JP.utf8"],
    undefined,
  );
  assert.strictEqual(langOf(result), "ja_JP.SJIS");
});

test("tries every combination of region candidates: no match via baseEnv.LANG's region, but matches via a KNOWN_GOOD_TERRITORY-derived region", () => {
  const base = { LANG: "en_US.UTF-8" }; // there's no en_US.GB18030
  const result = resolveLocaleEnv(
    "GB18030",
    base,
    ["en_US.utf8", "zh_CN.gb18030"],
    undefined,
  );
  assert.strictEqual(langOf(result), "zh_CN.GB18030");
});

test("fills in a region from uiLanguage (with a region): 'zh-cn' becomes zh_CN", () => {
  const base = {};
  const result = resolveLocaleEnv("GB18030", base, ["zh_CN.gb18030"], "zh-cn");
  assert.strictEqual(langOf(result), "zh_CN.GB18030");
});

test("uiLanguage (language only) is filled in via the language→representative-region table: 'ja' becomes ja_JP", () => {
  const base = {};
  const result = resolveLocaleEnv("euc-jp-2007", base, ["ja_JP.eucjp"], "ja");
  assert.strictEqual(langOf(result), "ja_JP.EUC-JP");
});

test("region candidate priority: baseEnv.LANG-derived > uiLanguage-derived > KNOWN_GOOD_TERRITORY. Uses the uiLanguage-derived region when that's the only one that matches", () => {
  const base = {}; // no baseEnv.LANG
  // ISO8859-1's KNOWN_GOOD_TERRITORY is de_DE, but simulate an environment
  // that only has fr_FR.iso88591. uiLanguage="fr" should derive fr_FR,
  // which should be the one that matches.
  const result = resolveLocaleEnv("ISO8859-1", base, ["fr_FR.iso88591"], "fr");
  assert.strictEqual(langOf(result), "fr_FR.ISO8859-1");
});

test("when availableLocales is an empty array (probe succeeded, 0 entries), every encoding reports noMatch", () => {
  const base = { LANG: "en_US.UTF-8" };
  for (const [enc, ui] of [
    ["euc-jp-2007", "ja"],
    ["GB18030", "zh-cn"],
    ["CP932", "ja"],
  ] as const) {
    assert.deepStrictEqual(resolveLocaleEnv(enc, base, [], ui), {
      kind: "noMatch",
    });
  }
});

test("when availableLocales is null (probing impossible), nothing is forced for any encoding, including euc-jp-2007", () => {
  const base = { LANG: "en_US.UTF-8" };
  for (const enc of ["euc-jp-2007", "GB18030", "CP932", "ISO8859-1"]) {
    assert.deepStrictEqual(resolveLocaleEnv(enc, base, null, "ja"), {
      kind: "probeFailed",
    });
  }
});

test("an inherited LC_* naming another encoding outranks LANG and is reported as a conflict, not overwritten", () => {
  const result = resolveLocaleEnv(
    "euc-jp-2007",
    { LANG: "ja_JP.UTF-8", LC_ALL: "en_US.UTF-8", LC_TIME: "ja_JP.UTF-8" },
    ["ja_JP.eucjp"],
    undefined,
  );
  assert.ok(result.kind === "matched");
  assert.strictEqual(result.locale, "ja_JP.EUC-JP");
  assert.deepStrictEqual(result.conflicts, ["LC_ALL", "LC_TIME"]);
});

test("no conflict for LC_* already in the target encoding, without a codeset, or C/POSIX in a non-ctype category", () => {
  const result = resolveLocaleEnv(
    "euc-jp-2007",
    {
      LANG: "ja_JP.UTF-8",
      LC_CTYPE: "ja_JP.eucJP",
      LC_TIME: "C.UTF-8",
      LC_MESSAGES: "POSIX",
      LC_NUMERIC: "ja_JP",
    },
    ["ja_JP.eucjp"],
    undefined,
  );
  assert.ok(result.kind === "matched");
  assert.deepStrictEqual(result.conflicts, []);
});

test("C.UTF-8 in LC_ALL/LC_CTYPE is a conflict: it makes the character encoding UTF-8", () => {
  const result = resolveLocaleEnv(
    "euc-jp-2007",
    { LC_CTYPE: "C.UTF-8" },
    ["ja_JP.eucjp"],
    "ja",
  );
  assert.ok(result.kind === "matched");
  assert.deepStrictEqual(result.conflicts, ["LC_CTYPE"]);
});

test("last resort: an installed locale in the encoding is found even when its region isn't among the candidates (no false 'not installed' warning)", () => {
  const result = resolveLocaleEnv(
    "ISO8859-1",
    { LANG: "ja_JP.UTF-8" },
    ["en_US.iso88591", "ja_JP.utf8"],
    "ja",
  );
  assert.strictEqual(langOf(result), "en_US.iso88591");
});

test("the region is taken from the locale actually in effect for LC_CTYPE (LC_ALL > LC_CTYPE > LANG)", () => {
  const result = resolveLocaleEnv(
    "ISO8859-1",
    { LANG: "de_DE.UTF-8", LC_ALL: "fr_FR.UTF-8" },
    ["de_DE.iso88591", "fr_FR.iso88591"],
    undefined,
  );
  assert.strictEqual(langOf(result), "fr_FR.ISO8859-1");
});

test("suggestLocaleName: suggests a standard locale where glibc has one, nothing otherwise", () => {
  assert.strictEqual(suggestLocaleName("euc-jp-2007"), "ja_JP.EUC-JP");
  assert.strictEqual(suggestLocaleName("GB18030"), "zh_CN.GB18030");
  assert.strictEqual(suggestLocaleName("CP932"), undefined);
  assert.strictEqual(suggestLocaleName("CP437"), undefined);
});
