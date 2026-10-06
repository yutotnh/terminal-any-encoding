#!/usr/bin/env python3
"""The terminal shows what VS Code's editor shows, for every encoding.

For each supported encoding (src/encodings.ts):

- Output: every byte sequence the editor decodes to one character (all
  single bytes, all 2-byte pairs, EUC-JP's 3-byte G3 sequences and
  GB18030's 4-byte ones) is decoded by luit and compared with the editor.
- Input: every character the editor can encode is typed into luit through a
  real PTY, and the bytes the shell receives are compared with what the
  editor would save.

The editor's side comes from iconv-lite, the library VS Code decodes files
with (tests/iconv_lite_oracle.js). The only intended differences:

- Typed text must come back as the bytes it was displayed from. iconv-lite's
  EUC-JP encoder doesn't do that for two characters (VS Code issue #48802,
  iconv-lite #145; what the wave-dash-unify extension corrects on save):
  ～ U+FF5E goes to 0xA1C1, not 0x8FA2B7, and № U+2116 to 0xADE2, not
  0x8FA2F1. With that, every EUC-JP character with several byte sequences
  is sent as glibc's EUC-JP-MS sends it. 〜 U+301C, which iconv-lite can't
  encode at all (macOS's input methods type it), is sent as the wave dash,
  EUC-JP 0xA1C1 or Shift JIS 0x8160, too.
- C1 controls (U+0080-U+009F) aren't compared: in ISO 8859 terminals they
  are control characters, not text.
- GB18030's 4-byte sequences between the BMP's and the supplementary planes'
  (linear index 39420-188999) and past U+10FFFF are unassigned, and invalid
  in the WHATWG Encoding Standard; iconv-lite decodes them anyway (the first
  one, 0x8431A530, as U+10000). They're left out.

Needs luit built (transcoder/src/luit), node, and `npm ci` (iconv-lite).

Encodings are checked in parallel, one process each (-j N to limit), with
a copy of luit taken at the start, so rebuilding meanwhile doesn't matter.

Usage:
    python3 tests/test_editor_parity.py [-j N] [encoding id ...]
"""
import concurrent.futures
import faulthandler
import json
import multiprocessing
import os
import shutil
import pty
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LUIT = REPO / "transcoder" / "src" / "luit"
ORACLE = REPO / "tests" / "iconv_lite_oracle.js"

# encoding id -> {bytes (hex): code point} decoded on purpose unlike the editor
DECODE_EXCEPTIONS: dict[str, dict[str, int]] = {}
# encoding id -> {code point: bytes (hex)} typed on purpose unlike the editor
ENCODE_EXCEPTIONS = {
    "eucjp": {0x301C: "a1c1", 0xFF5E: "a1c1", 0x2116: "ade2"},
    "shiftjis": {0x301C: "8160"},
}


def oracle(command: str, enc_id: str, lines: list[str]) -> list[str]:
    p = subprocess.run(
        ["node", str(ORACLE), command, enc_id],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        check=True,
    )
    out = p.stdout.split("\n")
    return out[: len(lines)]


def candidates(enc_id: str) -> list[bytes]:
    """Every sequence that could be one character: single bytes, then longer
    ones only in the shapes the editor actually decodes as one."""
    seqs = [bytes([b]) for b in range(0x20, 0x100) if b != 0x7F]
    seqs += [bytes([l, t]) for l in range(0x81, 0xFF) for t in range(0x21, 0xFF) if t != 0x7F]
    probe = oracle("decode", enc_id, ["8fa1a1", "81308130"])
    if len(probe[0].split()) == 1:  # EUC-JP's G3 (SS3 + 2 bytes)
        seqs += [bytes([0x8F, a, b]) for a in range(0xA1, 0xFF) for b in range(0xA1, 0xFF)]
    if len(probe[1].split()) == 1:  # GB18030's 4-byte sequences
        seqs += [
            bytes([a, b, c, d])
            for a in range(0x81, 0xFF)
            for b in range(0x30, 0x3A)
            for c in range(0x81, 0xFF)
            for d in range(0x30, 0x3A)
        ]
    return seqs


def gb18030_unassigned(seq: bytes) -> bool:
    if len(seq) != 4:
        return False
    b1, b2, b3, b4 = seq
    linear = ((b1 - 0x81) * 10 + (b2 - 0x30)) * 1260 + (b3 - 0x81) * 10 + (b4 - 0x30)
    return 39420 <= linear < 189000 or linear > 1237575


def expected_decodings(enc_id: str) -> dict[bytes, int]:
    """Sequences the editor decodes to exactly one comparable character."""
    seqs = candidates(enc_id)
    if enc_id == "gb18030":
        seqs = [s for s in seqs if not gb18030_unassigned(s)]
    decoded = oracle("decode", enc_id, [s.hex() for s in seqs])
    result: dict[bytes, int] = {}
    for seq, cps in zip(seqs, decoded):
        parts = cps.split()
        if len(parts) != 1:
            continue
        cp = int(parts[0], 16)
        if cp == 0xFFFD or cp < 0x20 or 0x7F <= cp <= 0x9F:
            continue
        result[seq] = cp
    for hexseq, cp in DECODE_EXCEPTIONS.get(enc_id, {}).items():
        result[bytes.fromhex(hexseq)] = cp
    return result


def luit_decode(enc: str, seqs: list[bytes]) -> list[str]:
    """Decodes many sequences in one run, one per line."""
    data = b"".join(s + b"\n" for s in seqs)
    p = subprocess.run([str(LUIT), "-c", "-encoding", enc], input=data, capture_output=True)
    return p.stdout.decode("utf-8", errors="replace").split("\n")


def luit_decode_one(enc: str, seq: bytes) -> str:
    p = subprocess.run([str(LUIT), "-c", "-encoding", enc], input=seq, capture_output=True)
    return p.stdout.decode("utf-8", errors="replace")


def fmt(text: str) -> str:
    return "+".join(f"U+{ord(c):04X}" for c in text) or "(nothing)"


def check_decoding(enc_id: str, enc: str, expected: dict[bytes, int]) -> list[str]:
    seqs = list(expected)
    got = luit_decode(enc, seqs)
    problems = []
    for i, seq in enumerate(seqs):
        want = chr(expected[seq])
        if i < len(got) and got[i] == want:
            continue
        # A batch can go out of step after a sequence luit reads differently;
        # what counts is the sequence on its own.
        alone = luit_decode_one(enc, seq)
        if alone != want:
            problems.append(f"{seq.hex()} -> {fmt(alone)}, editor {fmt(want)}")
    return problems


def expected_encodings(enc_id: str, decodings: dict[bytes, int]) -> dict[int, bytes]:
    chars = sorted(set(decodings.values()) - set(ENCODE_EXCEPTIONS.get(enc_id, {})))
    encoded = oracle("encode", enc_id, [f"{cp:x}" for cp in chars])
    result = {cp: bytes.fromhex(b) for cp, b in zip(chars, encoded) if b != "-"}
    for cp, hexbytes in ENCODE_EXCEPTIONS.get(enc_id, {}).items():
        result[cp] = bytes.fromhex(hexbytes)
    return result


def luit_encode(enc: str, text: str, expect_len: int) -> tuple[bytes, bool]:
    """Types text into luit through a PTY; returns the bytes the child got
    and whether luit rang the bell (rejected input)."""
    with tempfile.TemporaryDirectory() as tmp:
        out_path = os.path.join(tmp, "received.bin")
        pid, fd = pty.fork()
        if pid == 0:
            os.execvp(str(LUIT), ["luit", "-encoding", enc, "--", "sh", "-c",
                                  'stty raw -echo; exec cat > "$0"', out_path])
            os._exit(1)
        time.sleep(0.5)
        data = text.encode("utf-8")
        # Blocking writes from a thread: each one goes through as soon as
        # the pty takes it. Waiting in select() for writability instead was
        # 100 times slower on macOS, whose ptys have small buffers.
        written = threading.Event()

        def write_all() -> None:
            view = memoryview(data)
            pos = 0
            try:
                while pos < len(view):
                    pos += os.write(fd, view[pos:pos + 65536])
            except OSError:
                pass
            written.set()

        writer = threading.Thread(target=write_all, daemon=True)
        writer.start()
        bell = False
        size, last_change = -1, time.time()
        while True:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    if b"\a" in os.read(fd, 65536):
                        bell = True
                except OSError:
                    break
            now_size = os.path.getsize(out_path) if os.path.exists(out_path) else 0
            if now_size != size or not written.is_set():
                size, last_change = now_size, time.time()
            # Done when everything arrived, or when nothing has moved for a
            # while after the last write (rejected input never arrives).
            if written.is_set() and (size >= expect_len or time.time() - last_change > 1.5):
                time.sleep(0.2)
                break
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        writer.join()
        os.close(fd)
        got = open(out_path, "rb").read() if os.path.exists(out_path) else b""
    return got, bell


def check_encoding(enc: str, expected: dict[int, bytes], chars: list[int] | None = None,
                   limit: int = 20) -> list[str]:
    """All characters in one go; if anything differs, halves until it's
    found which ones (a rejected character drops the rest of its input)."""
    if chars is None:
        chars = sorted(expected)
    want = b"".join(expected[cp] for cp in chars)
    got, bell = luit_encode(enc, "".join(chr(cp) for cp in chars), len(want))
    if got == want:
        return []
    if len(chars) == 1:
        cp = chars[0]
        return [f"U+{cp:04X} sent {got.hex() or '(nothing)'}{' (rejected)' if bell else ''},"
                f" editor {expected[cp].hex()}"]
    half = len(chars) // 2
    problems = check_encoding(enc, expected, chars[:half], limit)
    if len(problems) < limit:
        problems += check_encoding(enc, expected, chars[half:], limit - len(problems))
    return problems


def check_one(luit: str, e: dict) -> tuple[str, bool]:
    """Checks one encoding (in a worker process); returns its report."""
    global LUIT
    LUIT = Path(luit)
    decodings = expected_decodings(e["id"])
    problems = check_decoding(e["id"], e["luitEncoding"], decodings)
    encodings = expected_encodings(e["id"], decodings)
    problems += check_encoding(e["luitEncoding"], encodings)
    lines = [("OK " if not problems else "NG ")
             + f"{e['id']:12s} {len(decodings):7d} sequences, {len(encodings):7d} characters"
             + (f": {len(problems)} differ" if problems else "")]
    lines += [f"      {p}" for p in problems[:10]]
    if len(problems) > 10:
        lines.append(f"      ... and {len(problems) - 10} more")
    return "\n".join(lines), bool(problems)


def main() -> int:
    sys.stdout.reconfigure(line_buffering=True)
    faulthandler.enable()
    args = sys.argv[1:]
    jobs = os.cpu_count() or 1
    if args[:1] == ["-j"]:
        jobs, args = int(args[1]), args[2:]
    if not LUIT.exists():
        print(f"NG: {LUIT} not found. Build the transcoder first.", file=sys.stderr)
        return 1
    encodings = json.loads(subprocess.run(
        ["node", str(ORACLE), "list"], capture_output=True, text=True, check=True).stdout)
    if args:
        encodings = [e for e in encodings if e["id"] in set(args)]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        luit = os.path.join(tmp, "luit")
        shutil.copy2(LUIT, luit)
        # spawn, not fork: the workers fork PTYs themselves
        with concurrent.futures.ProcessPoolExecutor(
                jobs, mp_context=multiprocessing.get_context("spawn")) as pool:
            futures = [pool.submit(check_one, luit, e) for e in encodings]
            for future in futures:
                report, failed = future.result()
                print(report)
                failures += failed
    print(f"\n{failures} encoding(s) differ from the editor")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
