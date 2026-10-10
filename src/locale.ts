/**
 * Decides the locale variables to pass to the shell running inside the
 * transcoder.
 *
 * Only a locale that's checked against the list of locales actually
 * generated on the host (`locale -a`, localeProbe.ts) is used. Setting a
 * nonexistent locale makes glibc print a warning and fall back to the C
 * locale, so nothing is forced when there's no match; the caller
 * tells the user instead.
 *
 * The result is applied by the transcoder's child (`env ... <shell>`), not
 * through the terminal's own environment, so VS Code's
 * `terminal.integrated.detectLocale` (which rewrites `LANG` of the process
 * it launches, i.e. the transcoder) can't override it.
 *
 * Only `LANG` is set, the same variable VS Code's own detectLocale sets.
 * Locale variables the user set explicitly (`LC_ALL`, `LC_CTYPE`,
 * `LC_TIME`, ...) are the user's configuration and outrank `LANG`; when one
 * of them points at a different encoding, it's reported back as a conflict
 * (the caller warns) instead of being overwritten.
 *
 * See docs/extension-design.md for the design background and real-device
 * verification results.
 */

/**
 * Candidate glibc charmap names per luitEncoding.
 *
 * The default is `[luitEncoding]` (treating the luitEncoding string itself
 * as the charmap name; this works for things like ISO8859-1 and eucKR, which
 * match glibc's actual notation (iso88591, euckr, etc.) once normalized
 * (lowercased + non-alphanumerics stripped)).
 *
 * The exceptions:
 *   - "euc-jp-2007": glibc's charmap is "EUC-JP".
 *   - "CP932": glibc has no standard charmap, but some distributions
 *     generate their own locales like `ja_JP.sjis`, so a few aliases are
 *     tried.
 *   - "MACROMAN": glibc's charmap is "MACINTOSH".
 */
const CODESET_ALIASES: Readonly<Record<string, readonly string[]>> = {
  "euc-jp-2007": ["EUC-JP"],
  CP932: ["CP932", "SJIS", "SHIFT_JIS", "MS_KANJI"],
  MACROMAN: ["MACROMAN", "MACINTOSH"],
};

/**
 * Per luitEncoding, a region glibc's SUPPORTED list has a locale for in
 * that encoding (`xx_YY`, without the charset part): the last region tried.
 * Encodings missing here (CP932, CP874 and the DOS code pages) have no such
 * locale in glibc, only ones some hosts generate themselves.
 */
const KNOWN_GOOD_TERRITORY: Readonly<Record<string, string>> = {
  "euc-jp-2007": "ja_JP",
  GB18030: "zh_CN",
  GBK: "zh_CN",
  GB2312: "zh_CN",
  Big5: "zh_TW",
  "BIG5-HKSCS": "zh_HK",
  eucKR: "ko_KR",
  "ISO8859-1": "de_DE",
  "ISO8859-2": "cs_CZ",
  "ISO8859-3": "mt_MT",
  "ISO8859-5": "ru_RU",
  "ISO8859-6": "ar_AE",
  "ISO8859-7": "el_GR",
  "ISO8859-8": "he_IL",
  "ISO8859-9": "tr_TR",
  "ISO8859-10": "lg_UG",
  "ISO8859-13": "lt_LT",
  "ISO8859-14": "cy_GB",
  "ISO8859-15": "et_EE",
  "KOI8-R": "ru_RU",
  "KOI8-U": "uk_UA",
  CP1251: "be_BY",
  CP1255: "yi_US",
  CP1250: "pl_PL",
  CP1252: "en_US",
  CP1253: "el_GR",
  CP1254: "tr_TR",
  CP1256: "ar_SA",
  CP1257: "lt_LT",
  CP1258: "vi_VN",
  CP1125: "uk_UA",
  "KOI8-RU": "uk_UA",
  "KOI8-T": "tg_TJ",
  "ISO8859-4": "lv_LV",
  "ISO8859-11": "th_TH",
  "ISO8859-16": "ro_RO",
  MACROMAN: "en_US",
};

/**
 * "Language → representative region" table, modeled after VS Code's own
 * getLangEnvVariable() implementation (terminalEnvironment.ts). Used to fill
 * in a region when `vscode.env.language` (e.g. "ja", "zh-cn") doesn't
 * include one.
 */
const LANGUAGE_TERRITORY: Readonly<Record<string, string>> = {
  af: "ZA",
  am: "ET",
  be: "BY",
  bg: "BG",
  ca: "ES",
  cs: "CZ",
  da: "DK",
  de: "DE",
  el: "GR",
  en: "US",
  es: "ES",
  et: "EE",
  eu: "ES",
  fi: "FI",
  fr: "FR",
  he: "IL",
  hr: "HR",
  hu: "HU",
  hy: "AM",
  is: "IS",
  it: "IT",
  ja: "JP",
  kk: "KZ",
  ko: "KR",
  lt: "LT",
  nl: "NL",
  no: "NO",
  pl: "PL",
  pt: "BR",
  ro: "RO",
  ru: "RU",
  sk: "SK",
  sl: "SI",
  sr: "YU",
  sv: "SE",
  tr: "TR",
  uk: "UA",
  zh: "CN",
};

/** Lowercase + strip non-alphanumerics, so `ja_JP.EUC-JP` and `locale -a`'s `ja_JP.eucjp` are treated as the same */
function normalize(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]/g, "");
}

/** Extracts an `xx_YY`-form region from a locale name (e.g. "ja_JP.UTF-8") */
function territoryFromEnvLang(lang: string | undefined): string | undefined {
  if (!lang) return undefined;
  const m = /^([a-zA-Z]{2,3})_([a-zA-Z]{2})\b/.exec(lang);
  if (!m) return undefined;
  return `${m[1].toLowerCase()}_${m[2].toUpperCase()}`;
}

/** Extracts/guesses an `xx_YY`-form region from `vscode.env.language` (e.g. "ja", "zh-cn") */
function territoryFromUiLanguage(
  uiLanguage: string | undefined,
): string | undefined {
  if (!uiLanguage) return undefined;
  const parts = uiLanguage.split("-");
  const lang = parts[0]?.toLowerCase();
  if (!lang) return undefined;
  if (parts[1]) {
    return `${lang}_${parts[1].toUpperCase()}`;
  }
  const territory = LANGUAGE_TERRITORY[lang];
  return territory ? `${lang}_${territory}` : undefined;
}

/** Returns the candidate regions in priority order, deduplicated */
function buildTerritoryCandidates(
  luitEncoding: string,
  baseEnv: Readonly<NodeJS.ProcessEnv>,
  uiLanguage: string | undefined,
): readonly string[] {
  const candidates = [
    territoryFromEnvLang(effectiveCtype(baseEnv)),
    territoryFromUiLanguage(uiLanguage),
    KNOWN_GOOD_TERRITORY[luitEncoding],
  ].filter((t): t is string => Boolean(t));
  return Array.from(new Set(candidates));
}

function codesetCandidates(luitEncoding: string): readonly string[] {
  return CODESET_ALIASES[luitEncoding] ?? [luitEncoding];
}

/** Variables that outrank `LANG` for some locale category */
const LC_VARIABLES: readonly string[] = [
  "LC_ALL",
  "LC_CTYPE",
  "LC_COLLATE",
  "LC_MESSAGES",
  "LC_MONETARY",
  "LC_NUMERIC",
  "LC_TIME",
  "LC_PAPER",
  "LC_NAME",
  "LC_ADDRESS",
  "LC_TELEPHONE",
  "LC_MEASUREMENT",
  "LC_IDENTIFICATION",
];

export type LocaleResolution =
  /**
   * A matching locale exists; the shell gets `LANG=<locale>`. `conflicts`
   * lists inherited `LC_*` variables naming a different encoding, which
   * outrank `LANG` and leave the shell (partly) on that encoding.
   */
  | {
      readonly kind: "matched";
      readonly locale: string;
      readonly conflicts: readonly string[];
    }
  /** Probing worked but the host has no matching locale */
  | { readonly kind: "noMatch" }
  /** `locale -a` itself couldn't run, so existence can't be confirmed */
  | { readonly kind: "probeFailed" };

/** The locale currently in effect for LC_CTYPE (LC_ALL > LC_CTYPE > LANG) */
function effectiveCtype(env: Readonly<NodeJS.ProcessEnv>): string | undefined {
  return env.LC_ALL || env.LC_CTYPE || env.LANG || undefined;
}

/** The codeset part of a locale name ("ja_JP.UTF-8@x" → "UTF-8"), if any */
function codesetOf(locale: string): string | undefined {
  return /\.([^@]+)/.exec(locale)?.[1];
}

/** C/POSIX (including C.UTF-8 etc.): ASCII-only output, fine under any encoding */
function isAsciiLocale(locale: string): boolean {
  return /^(C|POSIX)([.@]|$)/.test(locale);
}

/**
 * Decides the shell's `LANG` for luitEncoding.
 *
 * - availableLocales null (probing impossible, e.g. no `locale` command):
 *   `probeFailed`. Nothing is forced, for any encoding.
 * - Otherwise, cross-references candidate region × charset combinations
 *   against what was actually probed on the host and returns the first
 *   match (falling back to any installed locale in the encoding), or
 *   `noMatch`.
 */
export function resolveLocaleEnv(
  luitEncoding: string,
  baseEnv: Readonly<NodeJS.ProcessEnv>,
  availableLocales: readonly string[] | null,
  uiLanguage: string | undefined,
): LocaleResolution {
  if (availableLocales === null) return { kind: "probeFailed" };

  const available = new Set(availableLocales.map(normalize));
  const codesets = codesetCandidates(luitEncoding).map(normalize);
  const isTargetCodeset = (locale: string): boolean => {
    const codeset = codesetOf(locale);
    return codeset !== undefined && codesets.includes(normalize(codeset));
  };

  let matched: string | undefined;
  for (const territory of buildTerritoryCandidates(
    luitEncoding,
    baseEnv,
    uiLanguage,
  )) {
    matched = codesetCandidates(luitEncoding)
      .map((codeset) => `${territory}.${codeset}`)
      .find((candidate) => available.has(normalize(candidate)));
    if (matched) break;
  }
  // Last resort: any installed locale in this encoding, whatever its
  // region, so the user isn't told "no locale" when one exists.
  matched ??= availableLocales.find(isTargetCodeset);
  if (!matched) return { kind: "noMatch" };

  // Only variables that name a codeset can be judged (a bare "ja_JP" is
  // left alone). C/POSIX only produce ASCII, which is fine for every
  // category except the character encoding itself (C.UTF-8 there is UTF-8).
  const conflicts = LC_VARIABLES.filter((name) => {
    const value = baseEnv[name];
    if (!value || codesetOf(value) === undefined || isTargetCodeset(value))
      return false;
    const decidesEncoding = name === "LC_ALL" || name === "LC_CTYPE";
    return decidesEncoding || !isAsciiLocale(value);
  });
  return { kind: "matched", locale: matched, conflicts };
}

/**
 * A locale name worth suggesting to generate when there's no match, or
 * undefined when glibc has no standard locale for this encoding (see
 * KNOWN_GOOD_TERRITORY).
 */
export function suggestLocaleName(luitEncoding: string): string | undefined {
  const territory = KNOWN_GOOD_TERRITORY[luitEncoding];
  if (!territory) return undefined;
  return `${territory}.${codesetCandidates(luitEncoding)[0]}`;
}
