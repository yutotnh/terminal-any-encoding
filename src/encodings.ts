/**
 * The supported encodings, and validation of encoding ids.
 *
 * These are the encodings VS Code's files.encoding offers, except the
 * Unicode ones, and the transcoder converts each the way VS Code's editor
 * does (its tables come from iconv-lite; see docs/transcoder-design.md).
 * Variants VS Code doesn't have (e.g. EUC-JP-MS) aren't offered. An id not
 * listed here is an error: no terminal is opened for it.
 */

export interface EncodingDefinition {
  /** Stable identifier used in settings and command arguments */
  readonly id: string;
  /** Label shown in the QuickPick etc. (VS Code core's SUPPORTED_ENCODINGS labelLong) */
  readonly label: string;
  /** The `-encoding` value passed to the transcoder */
  readonly luitEncoding: string;
}

// In the files.encoding dropdown's order: VS Code core's SUPPORTED_ENCODINGS
// (vs/workbench/services/textfile/common/encoding.ts) without the Unicode
// entries (utf8/utf8bom/utf16le/utf16be; UTF-8 would just open the same
// terminal as `+`). Tests keep package.json's lists of encodings (the
// defaultEncoding setting's and the task type's) in this order.
export const ENCODINGS: readonly EncodingDefinition[] = [
  {
    id: "windows1252",
    label: "Western (Windows 1252)",
    luitEncoding: "CP1252",
  },
  { id: "iso88591", label: "Western (ISO 8859-1)", luitEncoding: "ISO8859-1" },
  { id: "iso88593", label: "Western (ISO 8859-3)", luitEncoding: "ISO8859-3" },
  {
    id: "iso885915",
    label: "Western (ISO 8859-15)",
    luitEncoding: "ISO8859-15",
  },
  { id: "macroman", label: "Western (Mac Roman)", luitEncoding: "MACROMAN" },
  { id: "cp437", label: "DOS (CP 437)", luitEncoding: "CP437" },
  { id: "windows1256", label: "Arabic (Windows 1256)", luitEncoding: "CP1256" },
  { id: "iso88596", label: "Arabic (ISO 8859-6)", luitEncoding: "ISO8859-6" },
  { id: "windows1257", label: "Baltic (Windows 1257)", luitEncoding: "CP1257" },
  { id: "iso88594", label: "Baltic (ISO 8859-4)", luitEncoding: "ISO8859-4" },
  {
    id: "iso885914",
    label: "Celtic (ISO 8859-14)",
    luitEncoding: "ISO8859-14",
  },
  {
    id: "windows1250",
    label: "Central European (Windows 1250)",
    luitEncoding: "CP1250",
  },
  {
    id: "iso88592",
    label: "Central European (ISO 8859-2)",
    luitEncoding: "ISO8859-2",
  },
  { id: "cp852", label: "Central European (CP 852)", luitEncoding: "CP852" },
  {
    id: "windows1251",
    label: "Cyrillic (Windows 1251)",
    luitEncoding: "CP1251",
  },
  { id: "cp866", label: "Cyrillic (CP 866)", luitEncoding: "CP866" },
  { id: "cp1125", label: "Cyrillic (CP 1125)", luitEncoding: "CP1125" },
  { id: "iso88595", label: "Cyrillic (ISO 8859-5)", luitEncoding: "ISO8859-5" },
  { id: "koi8r", label: "Cyrillic (KOI8-R)", luitEncoding: "KOI8-R" },
  { id: "koi8u", label: "Cyrillic (KOI8-U)", luitEncoding: "KOI8-U" },
  {
    id: "iso885913",
    label: "Estonian (ISO 8859-13)",
    luitEncoding: "ISO8859-13",
  },
  { id: "windows1253", label: "Greek (Windows 1253)", luitEncoding: "CP1253" },
  { id: "iso88597", label: "Greek (ISO 8859-7)", luitEncoding: "ISO8859-7" },
  { id: "windows1255", label: "Hebrew (Windows 1255)", luitEncoding: "CP1255" },
  { id: "iso88598", label: "Hebrew (ISO 8859-8)", luitEncoding: "ISO8859-8" },
  {
    id: "iso885910",
    label: "Nordic (ISO 8859-10)",
    luitEncoding: "ISO8859-10",
  },
  {
    id: "iso885916",
    label: "Romanian (ISO 8859-16)",
    luitEncoding: "ISO8859-16",
  },
  {
    id: "windows1254",
    label: "Turkish (Windows 1254)",
    luitEncoding: "CP1254",
  },
  { id: "iso88599", label: "Turkish (ISO 8859-9)", luitEncoding: "ISO8859-9" },
  { id: "cp857", label: "Turkish (CP 857)", luitEncoding: "CP857" },
  {
    id: "windows1258",
    label: "Vietnamese (Windows 1258)",
    luitEncoding: "CP1258",
  },
  { id: "gbk", label: "Simplified Chinese (GBK)", luitEncoding: "GBK" },
  {
    id: "gb18030",
    label: "Simplified Chinese (GB18030)",
    luitEncoding: "GB18030",
  },
  { id: "cp950", label: "Traditional Chinese (Big5)", luitEncoding: "Big5" },
  {
    id: "big5hkscs",
    label: "Traditional Chinese (Big5-HKSCS)",
    luitEncoding: "BIG5-HKSCS",
  },
  {
    id: "shiftjis",
    label: "Japanese (Shift JIS)",
    luitEncoding: "CP932",
  },
  {
    id: "eucjp",
    label: "Japanese (EUC-JP)",
    luitEncoding: "euc-jp-2007",
  },
  { id: "euckr", label: "Korean (EUC-KR)", luitEncoding: "eucKR" },
  { id: "windows874", label: "Thai (Windows 874)", luitEncoding: "CP874" },
  {
    id: "iso885911",
    label: "Latin/Thai (ISO 8859-11)",
    luitEncoding: "ISO8859-11",
  },
  { id: "koi8ru", label: "Cyrillic (KOI8-RU)", luitEncoding: "KOI8-RU" },
  { id: "koi8t", label: "Tajik (KOI8-T)", luitEncoding: "KOI8-T" },
  {
    id: "gb2312",
    label: "Simplified Chinese (GB 2312)",
    luitEncoding: "GB2312",
  },
  { id: "cp865", label: "Nordic DOS (CP 865)", luitEncoding: "CP865" },
  {
    id: "cp850",
    label: "Western European DOS (CP 850)",
    luitEncoding: "CP850",
  },
];

/**
 * The encoding's own name, without the region: the part of the label in
 * parentheses ("Japanese (EUC-JP)" → "EUC-JP"), or the whole label.
 */
export function encodingShortName(encoding: EncodingDefinition): string {
  return /\(([^)]+)\)\s*$/.exec(encoding.label)?.[1] ?? encoding.label;
}

/** How many recently used encodings the picker lists first */
export const RECENT_ENCODINGS_MAX = 3;

/**
 * The recently used list after using id: moved (or added) to the front,
 * unknown ids dropped, at most RECENT_ENCODINGS_MAX.
 */
export function recordRecentEncoding(
  recentIds: readonly string[],
  id: string,
): string[] {
  return [id, ...recentIds.filter((r) => r !== id)]
    .filter((r) => ENCODING_BY_ID.has(r))
    .slice(0, RECENT_ENCODINGS_MAX);
}

/**
 * The picker's two groups, like VS Code's Command Palette: the recently
 * used encodings, most recent first, then all the others in the usual
 * order (VS Code's files.encoding order).
 */
export function groupForPicker(recentIds: readonly string[]): {
  recent: EncodingDefinition[];
  others: EncodingDefinition[];
} {
  const recent = recentIds
    .map((id) => ENCODING_BY_ID.get(id))
    .filter((e): e is EncodingDefinition => e !== undefined)
    .slice(0, RECENT_ENCODINGS_MAX);
  return {
    recent,
    others: ENCODINGS.filter((e) => !recent.includes(e)),
  };
}

const ENCODING_BY_ID = new Map(ENCODINGS.map((e) => [e.id, e]));

export function getEncoding(id: string): EncodingDefinition | undefined {
  return ENCODING_BY_ID.get(id);
}

/** An unknown encoding id; the caller builds the message */
export interface UnknownEncodingReason {
  readonly id: string;
}

/** Looks up an encoding id; the caller tells the user when it's unknown */
export function validateEncodingId(
  id: string,
):
  | { ok: true; encoding: EncodingDefinition }
  | { ok: false; reason: UnknownEncodingReason } {
  const encoding = getEncoding(id);
  if (!encoding) {
    return { ok: false, reason: { id } };
  }
  return { ok: true, encoding };
}
