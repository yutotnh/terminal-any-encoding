#!/usr/bin/env python3
"""Generates the builtin charset table (builtin_ja.c) for the luit fork from ICU (uconv),
and from iconv-lite for the encodings ICU has no converter for (KOI8-T).

See docs/transcoder-design.md.

Usage:
    python3 gen_tables.py            # generates transcoder/src/builtin_ja.c
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
GOLDEN_DIR = HERE / "golden"
OUTPUT = REPO_ROOT / "transcoder" / "src" / "builtin_ja.c"
GB18030_RANGES_OUTPUT = REPO_ROOT / "transcoder" / "src" / "gb18030_ranges.c"
GB18030_RANGES_HEADER_OUTPUT = REPO_ROOT / "transcoder" / "src" / "gb18030_ranges.h"


def icu_version() -> str:
    out = subprocess.run(["icuinfo"], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if 'name="version"' in line and "unicode" not in line and "cldr" not in line:
            return line.split(">", 1)[1].split("<", 1)[0]
    raise RuntimeError("could not get the version from icuinfo")


def run_uconv(converter: str, input_bytes: bytes) -> bytes:
    # "substitute", not "skip": with skip, an invalid byte in a probed pair
    # silently disappears, so e.g. CP932 0xD8 0x80 (a 1-byte katakana plus an
    # invalid byte) came back as a single character and was recorded as a
    # bogus 2-byte mapping. substitute turns the invalid byte into U+FFFD, so
    # the result is no longer exactly one valid character and is dropped.
    return subprocess.run(
        ["uconv", "-f", converter, "-t", "UTF-8", "--callback", "substitute"],
        input=input_bytes,
        capture_output=True,
        check=True,
    ).stdout


def run_uconv_reverse(converter: str, input_bytes: bytes) -> bytes:
    """UTF-8 -> converter direction (the encode direction). Used when you want
    to know "how many bytes does this code point encode to", like in
    gen_plane_gb18030_4byte_ranges (the opposite direction of run_uconv)."""
    return subprocess.run(
        ["uconv", "-f", "UTF-8", "-t", converter, "--callback", "skip"],
        input=input_bytes,
        capture_output=True,
        check=True,
    ).stdout


def gen_plane_g1(converter: str) -> list[tuple[int, int]]:
    """Scans the 2-byte GR space (EUC form) and collects it converted to a GL 2-byte code (luit's internal representation)."""
    payload = bytearray()
    for b1 in range(0x80, 0x100):
        for b2 in range(0x21, 0x100):
            payload += bytes([b1, b2, 0x0A])
    out = run_uconv(converter, bytes(payload))
    lines = out.split(b"\n")
    rows = []
    i = 0
    for b1 in range(0x80, 0x100):
        for b2 in range(0x21, 0x100):
            if i < len(lines):
                try:
                    ch = lines[i].decode("utf-8")
                except UnicodeDecodeError:
                    ch = None
                if ch is not None and len(ch) == 1 and ch != "�":
                    if 0xA1 <= b1 <= 0xFE and 0xA1 <= b2 <= 0xFE:
                        gl = ((b1 & 0x7F) << 8) | (b2 & 0x7F)
                        rows.append((gl, ord(ch)))
            i += 1
    return rows


def gen_plane_g3(converter: str) -> list[tuple[int, int]]:
    """Scans the SS3 (8F xx yy, JIS X 0212) 3-byte space and collects it converted to a GL 2-byte code."""
    payload = bytearray()
    for b1 in range(0x21, 0x7F):
        for b2 in range(0x21, 0x7F):
            payload += bytes([0x8F, b1 | 0x80, b2 | 0x80, 0x0A])
    out = run_uconv(converter, bytes(payload))
    lines = out.split(b"\n")
    rows = []
    i = 0
    for b1 in range(0x21, 0x7F):
        for b2 in range(0x21, 0x7F):
            if i < len(lines):
                try:
                    ch = lines[i].decode("utf-8")
                except UnicodeDecodeError:
                    ch = None
                if ch is not None and len(ch) == 1 and ch != "�":
                    gl = (b1 << 8) | b2
                    rows.append((gl, ord(ch)))
            i += 1
    return rows


def gen_plane_raw1byte(converter: str) -> list[tuple[int, int]]:
    """Builds a lookup table keyed directly on a single-byte encoding
    Only covers 0x80-0xFF (0x00-0x7F is assumed to be ASCII,
    handled separately by T_128 on the charset.c side)."""
    payload = bytearray()
    for b in range(0x80, 0x100):
        payload += bytes([b, 0x0A])
    out = run_uconv(converter, bytes(payload))
    lines = out.split(b"\n")
    rows = []
    for i, b in enumerate(range(0x80, 0x100)):
        if i < len(lines):
            try:
                ch = lines[i].decode("utf-8")
            except UnicodeDecodeError:
                ch = None
            if ch is not None and len(ch) == 1 and ch != "�":
                rows.append((b, ord(ch)))
    return rows


def gen_plane_raw2byte(converter: str) -> list[tuple[int, int]]:
    """Collects the raw 2-byte space of things like SJIS directly as source (no coordinate conversion).

    Feeding an invalid lead byte (e.g. CP932's 0xFD/0xFE) through uconv
    --callback skip 2 bytes at a time can result in only the lead byte
    being skipped as invalid, while the following trail byte gets decoded
    independently as a standalone 1-byte character. This isn't a
    legitimate 2-byte mapping of (lead byte, trail byte) — it's an
    artifact that merely happens to coincide with the result of decoding
    the trail byte alone.
    So each trail byte is compared against decoding it standalone (solo),
    and if every entry for a given lead byte matches solo, that whole lead
    byte is treated as invalid (an artifact) and excluded."""
    trail_bytes = [b for b in range(0x40, 0x100) if b != 0x7F]

    solo_payload = bytearray()
    for b in trail_bytes:
        solo_payload += bytes([b, 0x0A])
    solo_out = run_uconv(converter, bytes(solo_payload)).split(b"\n")
    solo: dict[int, str] = {}
    for i, b in enumerate(trail_bytes):
        if i < len(solo_out):
            try:
                ch = solo_out[i].decode("utf-8")
            except UnicodeDecodeError:
                ch = None
            if ch is not None and len(ch) == 1 and ch != "�":
                solo[b] = ch

    payload = bytearray()
    pairs = []
    for b1 in range(0x81, 0xFF):
        for b2 in trail_bytes:
            pairs.append((b1, b2))
            payload += bytes([b1, b2, 0x0A])
    out = run_uconv(converter, bytes(payload))
    lines = out.split(b"\n")
    decoded: dict[int, list[tuple[int, str]]] = {}
    for idx, (b1, b2) in enumerate(pairs):
        if idx < len(lines):
            try:
                ch = lines[idx].decode("utf-8")
            except UnicodeDecodeError:
                ch = None
            if ch is not None and len(ch) == 1 and ch != "�":
                decoded.setdefault(b1, []).append((b2, ch))

    rows = []
    for b1, entries in decoded.items():
        if entries and all(solo.get(b2) == ch for b2, ch in entries):
            print(
                f"  warning: excluding lead byte 0x{b1:02X} of {converter} as invalid"
                " (an artifact that matches standalone trail-byte decoding across every entry)",
                file=sys.stderr,
            )
            continue
        for b2, ch in entries:
            rows.append(((b1 << 8) | b2, ord(ch)))
    return rows


def gen_plane_gb18030_4byte_ranges(converter: str) -> list[tuple[int, int]]:
    """Compactly extracts GB18030's 4-byte region (the BMP gaps + supplementary
    planes) as contiguous ranges of the linear index. Looking up the entire
    range one by one would produce over a million entries for the
    supplementary planes alone, so this uses a range table instead.

    The return value can't be squeezed into simple (source, target) pairs
    like (unicode_start<<32 | unicode_end, linear_start), so unlike the
    other planes, the caller assembles it separately. This returns a list
    of [(unicode_start, unicode_end, linear_start), ...] tuples (BMP only;
    the supplementary planes are handled by a single formula,
    codepoint-0x10000+189000, so they're not included in the table).
    """

    def linear(b: bytes) -> int:
        b1, b2, b3, b4 = b
        return ((b1 - 0x81) * 10 + (b2 - 0x30)) * 1260 + (b3 - 0x81) * 10 + (b4 - 0x30)

    cps = [cp for cp in range(0x80, 0x10000) if not (0xD800 <= cp <= 0xDFFF)]
    results: dict[int, bytes] = {}
    batch_size = 500
    i = 0
    while i < len(cps):
        chunk = cps[i : i + batch_size]
        payload = ("\x00".join(chr(c) for c in chunk)).encode("utf-8")
        out = run_uconv_reverse(converter, payload)
        parts = out.split(b"\x00")
        if len(parts) != len(chunk):
            raise RuntimeError(
                f"GB18030 4-byte range extraction: batch split count mismatch ({len(parts)} != {len(chunk)})."
                " The NUL-delimited assumption may no longer hold."
            )
        for cp, part in zip(chunk, parts):
            results[cp] = part
        i += batch_size

    ranges: list[tuple[int, int, int]] = []
    cur_start: int | None = None
    prev_cp: int | None = None
    prev_linear: int | None = None
    range_linear_start: int | None = None
    for cp in cps:
        b = results.get(cp)
        is4 = b is not None and len(b) == 4
        if is4:
            L = linear(b)
            if cur_start is None:
                cur_start = cp
                range_linear_start = L
            elif L != prev_linear + 1 or cp != prev_cp + 1:  # type: ignore[operator]
                ranges.append((cur_start, prev_cp, range_linear_start))  # type: ignore[arg-type]
                cur_start = cp
                range_linear_start = L
            prev_cp = cp
            prev_linear = L
        else:
            if cur_start is not None:
                ranges.append((cur_start, prev_cp, range_linear_start))  # type: ignore[arg-type]
                cur_start = None
    if cur_start is not None:
        ranges.append((cur_start, prev_cp, range_linear_start))  # type: ignore[arg-type]

    return ranges  # type: ignore[return-value]


def iconv_lite_version() -> str:
    pkg = REPO_ROOT / "node_modules" / "iconv-lite" / "package.json"
    return json.loads(pkg.read_text(encoding="utf-8"))["version"]


def gen_iconv_lite_raw1byte(encoding: str) -> list[tuple[int, int]]:
    """Builds a single-byte table by decoding each byte with iconv-lite, the
    library VS Code itself decodes files with. Only for encodings ICU has no
    converter for (KOI8-T); the version is pinned by package-lock.json, like
    ICU's by icu_version_expected. Same shape as gen_plane_raw1byte
    (0x80-0xFF)."""
    script = (
        "const iconv = require('iconv-lite');"
        "const enc = process.argv[1];"
        "if (!iconv.encodingExists(enc)) process.exit(2);"
        "const out = [];"
        "for (let b = 0x80; b < 0x100; b++) out.push(iconv.decode(Buffer.from([b]), enc));"
        "process.stdout.write(JSON.stringify(out));"
    )
    out = subprocess.run(
        ["node", "-e", script, encoding], cwd=REPO_ROOT, capture_output=True, text=True, check=True
    ).stdout
    rows = []
    for b, ch in zip(range(0x80, 0x100), json.loads(out)):
        # Unmapped bytes decode to U+FFFD.
        if len(ch) == 1 and ch != "\ufffd":
            rows.append((b, ord(ch)))
    return rows


def crosscheck_raw1byte(name: str, rows: list[tuple[int, int]], crosscheck: dict) -> None:
    """Fails unless the table matches every independent implementation
    listed (Python's codec, glibc's iconv) byte for byte. They're only
    compared against, never embedded (glibc's data is LGPL)."""
    expected = dict(rows)
    references = {}
    if "python" in crosscheck:
        table = {}
        for b in range(0x80, 0x100):
            try:
                table[b] = ord(bytes([b]).decode(crosscheck["python"]))
            except UnicodeDecodeError:
                pass
        references[f"Python {crosscheck['python']}"] = table
    if "glibc" in crosscheck:
        table = {}
        for b in range(0x80, 0x100):
            r = subprocess.run(
                ["iconv", "-f", crosscheck["glibc"], "-t", "UTF-8"], input=bytes([b]), capture_output=True
            )
            if r.returncode == 0:
                ch = r.stdout.decode("utf-8")
                if len(ch) == 1:
                    table[b] = ord(ch)
        if not table:
            raise RuntimeError(f"{name}: iconv doesn't know {crosscheck['glibc']}; install glibc's iconv to cross-check")
        references[f"glibc {crosscheck['glibc']}"] = table
    for label, table in references.items():
        if table != expected:
            diff = sorted(b for b in set(table) | set(expected) if table.get(b) != expected.get(b))
            raise RuntimeError(f"{name}: differs from {label} at {[hex(b) for b in diff]}")
        print(f"  {name}: matches {label}", file=sys.stderr)


def table_source(m: dict) -> str:
    return f"iconv-lite {m['iconv_lite']}" if "iconv_lite" in m else m["icu_converter"]


PLANE_GENERATORS = {
    "g1": gen_plane_g1,
    "g3": gen_plane_g3,
    "raw2byte": gen_plane_raw2byte,
    "raw1byte": gen_plane_raw1byte,
}


# Must match BUILTIN_DECODE_ONLY in transcoder/src/luitconv.h.
DECODE_ONLY = 0x80000000


def icu_bytes_to_source(plane: str, b: bytes) -> int | None:
    """Converts ICU's encoded bytes for one character into this table's
    source representation (the inverse of what each gen_plane_* does).
    Returns None if the bytes don't belong to this plane."""
    if plane == "g1" and len(b) == 2 and 0xA1 <= b[0] <= 0xFE and 0xA1 <= b[1] <= 0xFE:
        return ((b[0] & 0x7F) << 8) | (b[1] & 0x7F)
    if plane == "g3" and len(b) == 3 and b[0] == 0x8F:
        return ((b[1] & 0x7F) << 8) | (b[2] & 0x7F)
    if plane == "raw1byte" and len(b) == 1:
        return b[0]
    if plane == "raw2byte" and len(b) == 2:
        return (b[0] << 8) | b[1]
    return None


def mark_decode_only(name: str, plane: str, converter: str, rows: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """When several source codes decode to the same code point (e.g. CP932's
    U+FFE2 at 0x81CA/0xEEF9/0xFA54), luit's reverse lookup would pick one of
    them arbitrarily (bsearch over duplicates). Ask ICU which bytes it
    encodes that code point to, and flag every other duplicate as
    decode-only so it stays out of the reverse index. If ICU's choice isn't
    one of the candidates, leave the group alone and warn."""
    by_target: dict[int, list[int]] = {}
    for src, tgt in rows:
        by_target.setdefault(tgt, []).append(src)
    dups = sorted(t for t, srcs in by_target.items() if len(srcs) > 1)
    if not dups:
        return rows
    out = run_uconv_reverse(converter, "".join(chr(t) + "\n" for t in dups).encode("utf-8")).split(b"\n")
    decode_only: set[tuple[int, int]] = set()
    for tgt, encoded in zip(dups, out):
        canonical = icu_bytes_to_source(plane, encoded)
        if canonical not in by_target[tgt]:
            print(
                f"  warning: {name}: ICU encodes U+{tgt:04X} as {encoded.hex()}, which isn't one of"
                f" {[hex(s) for s in by_target[tgt]]}; leaving that group as-is",
                file=sys.stderr,
            )
            continue
        decode_only.update((src, tgt) for src in by_target[tgt] if src != canonical)
    return [(src, tgt | DECODE_ONLY) if (src, tgt) in decode_only else (src, tgt) for src, tgt in rows]


def format_target(tgt: int) -> str:
    if tgt & DECODE_ONLY:
        return "DECODE_ONLY(0x%04X)" % (tgt & ~DECODE_ONLY)
    return "0x%04X" % tgt


def c_identifier(name: str) -> str:
    return "tbl_" + "".join(c if c.isalnum() else "_" for c in name)


def render_builtin_ja_c(
    tables: dict[str, list[tuple[int, int]]], meta: list[dict], version: str, iconv_lite: str | None
) -> str:
    lines = []
    lines.append("/*")
    lines.append(" * builtin_ja.c -- additional builtin charset tables (fork-local, not upstream)")
    lines.append(" *")
    lines.append(" * GENERATED FILE - do not edit by hand.")
    lines.append(" * Generated by tools/gen-tables/gen_tables.py from ICU (uconv) converters,")
    lines.append(" * and from iconv-lite for the encodings ICU has no converter for.")
    lines.append(f" * ICU version at generation time: {version}")
    if iconv_lite:
        lines.append(f" * iconv-lite version at generation time: {iconv_lite}")
    lines.append(" * See tools/gen-tables/converters.json for the declarative source list,")
    lines.append(" * and docs/transcoder-design.md for the design rationale.")
    lines.append(" *")
    lines.append(" * Licensing: mapping data is derived from ICU (Unicode License V3,")
    lines.append(" * Copyright (c) 2016-2025 Unicode, Inc.) and iconv-lite (MIT,")
    lines.append(" * Copyright (c) 2011 Alexander Shtuchkin) -- see THIRD-PARTY-NOTICES.md.")
    lines.append(" * No glibc-derived data is used.")
    lines.append(" */")
    lines.append("#include <other.h>")
    lines.append("#include <sys.h>")
    lines.append("#include <luitconv.h>")
    lines.append("")
    lines.append("/* A source code that decodes to the same code point as another one, but")
    lines.append(" * that ICU doesn't encode that code point to, is decode-only. */")
    lines.append("#define DECODE_ONLY(ucs) (BUILTIN_DECODE_ONLY | (ucs))")
    lines.append("")
    lines.append("/* *INDENT-OFF* */")
    for m in meta:
        ident = c_identifier(m["name"])
        rows = tables[m["name"]]
        n_override = len(m.get("overrides", []))
        override_note = f", +{n_override} overrides (applied in array order, last wins)" if n_override else ""
        lines.append(f"/* {m['name']}: source={table_source(m)} plane={m['plane']} entries={len(rows)}{override_note}")
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


def render_gb18030_ranges_c(ranges: list[tuple[int, int, int]], converter: str, version: str) -> str:
    lines = []
    lines.append("/*")
    lines.append(" * gb18030_ranges.c -- GB18030 4-byte range table (fork-local, not upstream)")
    lines.append(" *")
    lines.append(" * GENERATED FILE - do not edit by hand.")
    lines.append(" * Generated by tools/gen-tables/gen_tables.py from ICU (uconv) converter"
                  f" {converter}.")
    lines.append(f" * ICU version at generation time: {version}")
    lines.append(" *")
    lines.append(" * Instead of fully expanding GB18030's 4-byte region (the BMP gaps +")
    lines.append(" * supplementary planes), this holds it compactly as contiguous ranges of")
    lines.append(" * the linear index. The supplementary planes (U+10000-U+10FFFF) are")
    lines.append(" * handled by a single formula (linear = cp-0x10000+189000) on the")
    lines.append(" * other_ja.c side, so they're not included in this table.")
    lines.append(" *")
    lines.append(" * Licensing: derived from ICU (Unicode License V3). See THIRD-PARTY-NOTICES.md.")
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


def build() -> tuple[str, dict[str, str]]:
    decl = json.loads(CONVERTERS_JSON.read_text(encoding="utf-8"))
    version = icu_version()
    expected = decl.get("icu_version_expected")
    if expected and version != expected:
        print(
            f"warning: the ICU version differs from what's expected (expected {expected}, actual {version})."
            " The table contents may change as a result.",
            file=sys.stderr,
        )

    tables: dict[str, list[tuple[int, int]]] = {}
    for m in decl["tables"]:
        if "iconv_lite" in m:
            if m["plane"] != "raw1byte":
                raise RuntimeError(f"{m['name']}: iconv-lite tables are raw1byte only")
            rows = sorted(gen_iconv_lite_raw1byte(m["iconv_lite"]))
            targets = [tgt for _, tgt in rows]
            if len(targets) != len(set(targets)):
                # Without ICU there's no way to tell which duplicate encodes.
                raise RuntimeError(f"{m['name']}: duplicate targets; decode-only marking needs ICU")
        else:
            gen = PLANE_GENERATORS[m["plane"]]
            rows = gen(m["icu_converter"])
            rows.sort()
            rows = mark_decode_only(m["name"], m["plane"], m["icu_converter"], rows)
        if "crosscheck" in m:
            crosscheck_raw1byte(m["name"], rows, m["crosscheck"])
        overrides = m.get("overrides", [])
        override_rows = [(int(o["source"], 16), int(o["target"], 16)) for o in overrides]
        # Override rows go "after" the base rows.
        # initializeBuiltInTable() (luitconv.c) processes the array in
        # order from the start, and decoding (table_utf8[source]) has "the
        # last entry with the same source wins", so putting the override
        # at the end lets it override the decode result. Meanwhile, the
        # encode-direction reverse lookup table (rev_index) accumulates
        # every entry unconditionally, so both the base row's target and
        # the override row's target are accepted when encoding (e.g. the
        # wave dash position can be encoded as either U+301C (override) or
        # U+FF5E (base)). This asymmetry is what the overrides rely on.
        # See docs/transcoder-design.md for details.
        rows = rows + override_rows
        tables[m["name"]] = rows
        extra = f" (+{len(override_rows)} overrides)" if override_rows else ""
        print(f"  {m['name']:20s} ({table_source(m)}, {m['plane']:9s}) -> {len(rows)} entries{extra}", file=sys.stderr)

    uses_iconv_lite = any("iconv_lite" in m for m in decl["tables"])
    source = render_builtin_ja_c(
        tables, decl["tables"], version, iconv_lite_version() if uses_iconv_lite else None
    )

    gb18030_source = None
    gb18030_ranges: list[tuple[int, int, int]] = []
    gb18030_decl = decl.get("gb18030_4byte_ranges")
    if gb18030_decl:
        gb18030_ranges = gen_plane_gb18030_4byte_ranges(gb18030_decl["icu_converter"])
        print(f"  gb18030_bmp_ranges   ({gb18030_decl['icu_converter']}, 4byte-ranges) -> {len(gb18030_ranges)} ranges", file=sys.stderr)
        gb18030_source = render_gb18030_ranges_c(gb18030_ranges, gb18030_decl["icu_converter"], version)

    hashes = {}
    for m in decl["tables"]:
        h = hashlib.sha256()
        for src, tgt in tables[m["name"]]:
            h.update(f"{src:04X} {tgt:04X}\n".encode("ascii"))
        hashes[m["name"]] = h.hexdigest()
    hashes["builtin_ja.c"] = hashlib.sha256(source.encode("utf-8")).hexdigest()

    if gb18030_source is not None:
        h = hashlib.sha256()
        for us, ue, ls in gb18030_ranges:
            h.update(f"{us:05X} {ue:05X} {ls}\n".encode("ascii"))
        hashes["gb18030_bmp_ranges"] = h.hexdigest()
        hashes["gb18030_ranges.c"] = hashlib.sha256(gb18030_source.encode("utf-8")).hexdigest()

    return source, hashes, gb18030_source


def main() -> int:
    check_only = "--check" in sys.argv
    if check_only:
        # The golden hashes (and builtin_ja.c's header) are tied to one ICU
        # version, so a different ICU can never pass. Say so up front instead
        # of reporting a wall of hash mismatches.
        expected = json.loads(CONVERTERS_JSON.read_text(encoding="utf-8")).get("icu_version_expected")
        version = icu_version()
        if expected and version != expected:
            print(
                f"NG: ICU {version} is installed, but the golden hashes were generated with ICU {expected}"
                " (icu_version_expected in converters.json). Run this with that ICU version.",
                file=sys.stderr,
            )
            return 1
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
        if ok:
            print("OK: every table matched the golden hash.", file=sys.stderr)
            return 0
        return 1

    OUTPUT.write_text(source, encoding="utf-8")
    print(f"generated: {OUTPUT} ({len(source)} bytes)", file=sys.stderr)

    if gb18030_source is not None:
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
