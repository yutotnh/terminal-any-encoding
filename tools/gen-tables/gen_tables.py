#!/usr/bin/env python3
"""Generates the transcoder's built-in charset tables (builtin_ja.c and
gb18030_ranges.c) from iconv-lite, the library VS Code decodes and encodes
files with, so the terminal shows what the editor shows.

Every byte sequence of a table's shape is decoded on its own, and a row
encodes (is in luit's reverse index) only when its bytes are what the
character should be sent as: iconv-lite's choice, with the corrections in
converters.json.

See docs/transcoder-design.md.

Usage:
    python3 gen_tables.py            # generates the tables and the golden hashes
    python3 gen_tables.py --check    # only verifies the output matches golden/tables.sha256 (for CI)
"""
import hashlib
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
CONVERTERS_JSON = HERE / "converters.json"
ICONV_LITE = HERE / "iconv_lite.js"
GOLDEN_DIR = HERE / "golden"
OUTPUT = REPO_ROOT / "transcoder" / "src" / "builtin_ja.c"
GB18030_RANGES_OUTPUT = REPO_ROOT / "transcoder" / "src" / "gb18030_ranges.c"
GB18030_RANGES_HEADER_OUTPUT = REPO_ROOT / "transcoder" / "src" / "gb18030_ranges.h"

# Must match BUILTIN_DECODE_ONLY in transcoder/src/luitconv.h.
DECODE_ONLY = 0x80000000


def iconv_lite(command: str, encoding: str = "", lines: list[str] | None = None) -> list[str]:
    args = ["node", str(ICONV_LITE), command] + ([encoding] if encoding else [])
    text = "\n".join(lines) + "\n" if lines else ""
    out = subprocess.run(args, cwd=REPO_ROOT, input=text, capture_output=True, text=True, check=True).stdout
    return out.split("\n")[: len(lines)] if lines is not None else [out]


def iconv_lite_version() -> str:
    return iconv_lite("version")[0]


def plane_sequences(plane: str) -> list[tuple[int, bytes]]:
    """Every byte sequence of the plane's shape, with the source value luit
    looks it up by:
    - raw1byte: the byte, all 256 of them (T_128 single-byte sets, shift
      0x80, and ISO 8859's T_96, whose upstream tables are keyed the same
      way; luit's "ASCII" is iso8859-1's GL half, which upstream left to an
      identity fallback)
    - raw2byte: the raw byte or 2-byte value (OTHER charsets such as CP932
      and GBK, and Big5's T_94192)
    - g1: an EUC 2-byte GR sequence, as GL (the high bit of both bytes stripped)
    - g3: EUC-JP's SS3 (0x8F) + 2 bytes, as GL
    - kana: EUC-JP's SS2 (0x8E) + 1 byte, as the GR byte (JIS X 0201:GR)"""
    gr = range(0xA1, 0xFF)
    if plane == "raw1byte":
        return [(b, bytes([b])) for b in range(0x100)]
    if plane == "raw2byte":
        singles = [(b, bytes([b])) for b in range(0x80, 0x100)]
        pairs = [
            ((l << 8) | t, bytes([l, t]))
            for l in range(0x81, 0xFF)
            for t in range(0x40, 0xFF)
            if t != 0x7F
        ]
        return singles + pairs
    if plane == "g1":
        return [(((a & 0x7F) << 8) | (b & 0x7F), bytes([a, b])) for a in gr for b in gr]
    if plane == "g3":
        return [(((a & 0x7F) << 8) | (b & 0x7F), bytes([0x8F, a, b])) for a in gr for b in gr]
    if plane == "kana":
        return [(b, bytes([0x8E, b])) for b in range(0xA1, 0xE0)]
    raise RuntimeError(f"unknown plane {plane}")


def row_bytes(plane: str, src: int) -> bytes:
    """The bytes a row stands for (the inverse of plane_sequences)."""
    if plane == "raw1byte":
        return bytes([src])
    if plane == "raw2byte":
        return bytes([src]) if src < 0x100 else bytes([src >> 8, src & 0xFF])
    if plane == "g1":
        return bytes([(src >> 8) | 0x80, (src & 0xFF) | 0x80])
    if plane == "g3":
        return bytes([0x8F, (src >> 8) | 0x80, (src & 0xFF) | 0x80])
    if plane == "kana":
        return bytes([0x8E, src])
    raise RuntimeError(f"unknown plane {plane}")


def gen_rows(encoding: str, plane: str) -> list[tuple[int, int]]:
    """Decodes every sequence of the plane; keeps those that are exactly one
    character (iconv-lite gives U+FFFD, or more than one character, for the
    others)."""
    seqs = plane_sequences(plane)
    decoded = iconv_lite("decode", encoding, [b.hex() for _, b in seqs])
    rows = []
    for (src, _), cps in zip(seqs, decoded):
        parts = cps.split()
        if len(parts) == 1 and parts[0] != "fffd":
            rows.append((src, int(parts[0], 16)))
    return rows


def mark_decode_only(
    encoding: str, plane: str, rows: list[tuple[int, int]], corrections: dict[str, str]
) -> list[tuple[int, int]]:
    """A row encodes only when its bytes are what its character should be
    sent as: iconv-lite's choice (VS Code saves it that way), unless
    converters.json corrects it. Other rows are decode-only: duplicates
    (luit's reverse lookup would otherwise pick one of them arbitrarily),
    rows whose character is sent from another table (e.g. EUC-JP's IBM
    extension kanji, sent as JIS X 0212 rather than from JIS X 0208's NEC
    rows), and characters the editor can't save at all."""
    chars = sorted({tgt for _, tgt in rows})
    encoded = iconv_lite("encode", encoding, [f"{cp:x}" for cp in chars])
    canonical = {cp: (None if b == "-" else bytes.fromhex(b)) for cp, b in zip(chars, encoded)}
    for cp_hex, hexbytes in corrections.items():
        canonical[int(cp_hex.removeprefix("U+"), 16)] = bytes.fromhex(hexbytes)
    return [
        (src, tgt) if canonical.get(tgt) == row_bytes(plane, src) else (src, tgt | DECODE_ONLY)
        for src, tgt in rows
    ]


def gen_gb18030_4byte_ranges(encoding: str) -> list[tuple[int, int, int]]:
    """GB18030's 4-byte sequences for the BMP, as contiguous ranges of the
    linear index: [(unicode_start, unicode_end, linear_start), ...]. Over a
    million entries one by one otherwise. The supplementary planes are a
    single formula (codepoint - 0x10000 + 189000) on the other_ja.c side."""

    def linear(b: bytes) -> int:
        b1, b2, b3, b4 = b
        return ((b1 - 0x81) * 10 + (b2 - 0x30)) * 1260 + (b3 - 0x81) * 10 + (b4 - 0x30)

    cps = [cp for cp in range(0x80, 0x10000) if not (0xD800 <= cp <= 0xDFFF)]
    encoded = iconv_lite("encode", encoding, [f"{cp:x}" for cp in cps])
    ranges: list[tuple[int, int, int]] = []
    start = prev_cp = prev_linear = range_linear_start = None
    for cp, hexbytes in zip(cps, encoded):
        b = bytes.fromhex(hexbytes) if hexbytes != "-" else b""
        if len(b) == 4:
            lin = linear(b)
            if start is None:
                start, range_linear_start = cp, lin
            elif lin != prev_linear + 1 or cp != prev_cp + 1:
                ranges.append((start, prev_cp, range_linear_start))
                start, range_linear_start = cp, lin
            prev_cp, prev_linear = cp, lin
        elif start is not None:
            ranges.append((start, prev_cp, range_linear_start))
            start = None
    if start is not None:
        ranges.append((start, prev_cp, range_linear_start))
    return ranges


def format_target(tgt: int) -> str:
    if tgt & DECODE_ONLY:
        return "DECODE_ONLY(0x%04X)" % (tgt & ~DECODE_ONLY)
    return "0x%04X" % tgt


def c_identifier(name: str) -> str:
    return "tbl_" + "".join(c if c.isalnum() else "_" for c in name)


LICENSE_NOTE = [
    " * Licensing: mapping data is derived from iconv-lite (MIT, Copyright (c)",
    " * 2011 Alexander Shtuchkin), whose tables come from the WHATWG Encoding",
    " * Standard and the Unicode Consortium's mapping files -- see",
    " * THIRD-PARTY-NOTICES.md. No glibc-derived data is used.",
]


def render_builtin_ja_c(tables: dict[str, list[tuple[int, int]]], meta: list[dict], version: str) -> str:
    lines = []
    lines.append("/*")
    lines.append(" * builtin_ja.c -- additional builtin charset tables (fork-local, not upstream)")
    lines.append(" *")
    lines.append(" * GENERATED FILE - do not edit by hand.")
    lines.append(" * Generated by tools/gen-tables/gen_tables.py from iconv-lite, the library")
    lines.append(" * VS Code decodes and encodes files with.")
    lines.append(f" * iconv-lite version at generation time: {version}")
    lines.append(" * See tools/gen-tables/converters.json for the declarative source list,")
    lines.append(" * and docs/transcoder-design.md for the design rationale.")
    lines.append(" *")
    lines += LICENSE_NOTE
    lines.append(" */")
    lines.append("#include <other.h>")
    lines.append("#include <sys.h>")
    lines.append("#include <luitconv.h>")
    lines.append("")
    lines.append("/* A row whose bytes aren't what its code point is sent as is decode-only:")
    lines.append(" * it stays out of luit's reverse index. */")
    lines.append("#define DECODE_ONLY(ucs) (BUILTIN_DECODE_ONLY | (ucs))")
    lines.append("")
    lines.append("/* *INDENT-OFF* */")
    for m in meta:
        ident = c_identifier(m["name"])
        rows = tables[m["name"]]
        n_override = len(m.get("overrides", []))
        override_note = f", +{n_override} overrides (applied in array order, last wins)" if n_override else ""
        lines.append(f"/* {m['name']}: iconv-lite {m['encoding']} plane={m['plane']} entries={len(rows)}{override_note}")
        lines.append(f" * {m['comment']} */")
        lines.append(f"static const BuiltInMapping {ident}[] =")
        lines.append("{")
        for src, tgt in rows:
            lines.append("    {0x%04X, %s}," % (src, format_target(tgt)))
        lines.append("};")
        lines.append("")
    lines.append("#define DATA(name) name, SizeOf(name)")
    lines.append("const BuiltInCharsetRec builtin_encodings_ja[] =")
    lines.append("{")
    for m in meta:
        ident = c_identifier(m["name"])
        lines.append(f'    {{ "{m["name"]}", DATA({ident}) }},')
    lines.append("    { NULL, NULL, 0 }")
    lines.append("};")
    lines.append("/* *INDENT-ON* */")
    lines.append("")
    return "\n".join(lines)


def render_gb18030_ranges_c(ranges: list[tuple[int, int, int]], encoding: str, version: str) -> str:
    lines = []
    lines.append("/*")
    lines.append(" * gb18030_ranges.c -- GB18030 4-byte range table (fork-local, not upstream)")
    lines.append(" *")
    lines.append(" * GENERATED FILE - do not edit by hand.")
    lines.append(f" * Generated by tools/gen-tables/gen_tables.py from iconv-lite's {encoding}.")
    lines.append(f" * iconv-lite version at generation time: {version}")
    lines.append(" *")
    lines.append(" * Instead of fully expanding GB18030's 4-byte region (the BMP gaps +")
    lines.append(" * supplementary planes), this holds it compactly as contiguous ranges of")
    lines.append(" * the linear index. The supplementary planes (U+10000-U+10FFFF) are")
    lines.append(" * handled by a single formula (linear = cp-0x10000+189000) on the")
    lines.append(" * other_ja.c side, so they're not included in this table.")
    lines.append(" *")
    lines += LICENSE_NOTE
    lines.append(" */")
    lines.append("#include \"gb18030_ranges.h\"")
    lines.append("")
    lines.append("/* *INDENT-OFF* */")
    lines.append(f"const Gb18030Range gb18030_bmp_ranges[{len(ranges)}] =")
    lines.append("{")
    for us, ue, ls in ranges:
        lines.append("    {0x%05X, 0x%05X, %d}," % (us, ue, ls))
    lines.append("};")
    lines.append(f"const unsigned gb18030_bmp_ranges_count = {len(ranges)};")
    lines.append("/* *INDENT-ON* */")
    lines.append("")
    return "\n".join(lines)


GB18030_RANGES_HEADER = """/*
 * gb18030_ranges.h -- GB18030 4-byte range table declaration (fork-local)
 */
#ifndef GB18030_RANGES_H
#define GB18030_RANGES_H

typedef struct {
    unsigned unicode_start;
    unsigned unicode_end;
    unsigned linear_start;
} Gb18030Range;

extern const Gb18030Range gb18030_bmp_ranges[];
extern const unsigned gb18030_bmp_ranges_count;

#endif
"""


def build() -> tuple[str, dict[str, str], str]:
    decl = json.loads(CONVERTERS_JSON.read_text(encoding="utf-8"))
    version = iconv_lite_version()
    corrections = decl.get("encode_corrections", {})

    tables: dict[str, list[tuple[int, int]]] = {}
    for m in decl["tables"]:
        rows = sorted(gen_rows(m["encoding"], m["plane"]))
        # Override rows go "after" the base rows. initializeBuiltInTable()
        # (luitconv.c) processes the array in order, and decoding
        # (table_utf8[source]) has "the last entry with the same source
        # wins", so the override decides what's displayed; whether a row
        # encodes is decided like for any other row (mark_decode_only).
        # See docs/transcoder-design.md for details.
        overrides = [(int(o["source"], 16), int(o["target"], 16)) for o in m.get("overrides", [])]
        rows = mark_decode_only(m["encoding"], m["plane"], rows + overrides, corrections.get(m["encoding"], {}))
        tables[m["name"]] = rows
        extra = f" (+{len(overrides)} overrides)" if overrides else ""
        print(f"  {m['name']:20s} ({m['encoding']}, {m['plane']:8s}) -> {len(rows)} entries{extra}", file=sys.stderr)

    source = render_builtin_ja_c(tables, decl["tables"], version)

    gb18030_encoding = decl["gb18030_4byte_ranges"]["encoding"]
    gb18030_ranges = gen_gb18030_4byte_ranges(gb18030_encoding)
    print(f"  gb18030_bmp_ranges   ({gb18030_encoding}, 4-byte ranges) -> {len(gb18030_ranges)} ranges", file=sys.stderr)
    gb18030_source = render_gb18030_ranges_c(gb18030_ranges, gb18030_encoding, version)

    hashes = {}
    for m in decl["tables"]:
        h = hashlib.sha256()
        for src, tgt in tables[m["name"]]:
            h.update(f"{src:04X} {tgt:04X}\n".encode("ascii"))
        hashes[m["name"]] = h.hexdigest()
    hashes["builtin_ja.c"] = hashlib.sha256(source.encode("utf-8")).hexdigest()
    h = hashlib.sha256()
    for us, ue, ls in gb18030_ranges:
        h.update(f"{us:05X} {ue:05X} {ls}\n".encode("ascii"))
    hashes["gb18030_bmp_ranges"] = h.hexdigest()
    hashes["gb18030_ranges.c"] = hashlib.sha256(gb18030_source.encode("utf-8")).hexdigest()

    return source, hashes, gb18030_source


def main() -> int:
    check_only = "--check" in sys.argv
    source, hashes, gb18030_source = build()

    golden_path = GOLDEN_DIR / "tables.sha256"
    if check_only:
        if not golden_path.exists():
            print(f"NG: golden file does not exist: {golden_path}", file=sys.stderr)
            return 1
        recorded = {}
        for line in golden_path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            h, name = line.split(None, 1)
            recorded[name.strip()] = h.strip()
        ok = True
        for name, h in hashes.items():
            if recorded.get(name) != h:
                print(f"NG: {name}'s hash doesn't match golden (current {h}, golden {recorded.get(name)})", file=sys.stderr)
                ok = False
        for name in sorted(set(recorded) - set(hashes)):
            print(f"NG: golden lists {name}, which is no longer generated", file=sys.stderr)
            ok = False
        if ok:
            print("OK: every table matched the golden hash.", file=sys.stderr)
            return 0
        return 1

    OUTPUT.write_text(source, encoding="utf-8")
    print(f"generated: {OUTPUT} ({len(source)} bytes)", file=sys.stderr)
    GB18030_RANGES_OUTPUT.write_text(gb18030_source, encoding="utf-8")
    GB18030_RANGES_HEADER_OUTPUT.write_text(GB18030_RANGES_HEADER, encoding="utf-8")
    print(f"generated: {GB18030_RANGES_OUTPUT} ({len(gb18030_source)} bytes)", file=sys.stderr)

    GOLDEN_DIR.mkdir(exist_ok=True)
    with golden_path.open("w", encoding="utf-8") as f:
        for name, h in hashes.items():
            f.write(f"{h}  {name}\n")
    print(f"golden updated: {golden_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
