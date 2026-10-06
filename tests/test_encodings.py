#!/usr/bin/env python3
"""Table-driven tests for the transcoder.

Checks the transcoder's conversions in both directions (output decoding and
input encoding through a real PTY), the fallback behavior, and regressions
for the upstream bugs the fork fixed.
Assumes `transcoder/src/luit` has already been built.

Usage:
    cd transcoder/src && ./configure --disable-fontenc && make
    python3 tests/test_encodings.py
"""
import faulthandler
import fcntl
import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import tty
from pathlib import Path
import time

REPO_ROOT = Path(__file__).resolve().parent.parent
LUIT = REPO_ROOT / "transcoder" / "src" / "luit"

# (encoding, input byte sequence (hex), expected code point, description) -- output direction
OUTPUT_CASES = [
    ("euc-jp-2007", "fce2", 0x9AD9, "髙 (G1)"),
    ("euc-jp-2007", "8fecbf", 0x9DD7, "鷗 (G3/SS3, JIS X 0212)"),
    ("euc-jp-2007", "ada1", 0x2460, "① (G1, NEC special character)"),
    ("euc-jp-2007", "a1c1", 0xFF5E, "wave dash position, shown as VS Code shows it (U+FF5E)"),
    ("euc-jp-2007", "c6fccbdc", None, "日本 (basic JIS X0208; string comparison done separately)"),
    ("CP932", "fbfc", 0x9AD9, "髙 (direct lookup)"),
    ("CP932", "8740", 0x2460, "① (NEC special character)"),
    ("CP932", "8160", 0xFF5E, "wave dash position, shown as VS Code shows it (U+FF5E)"),
    ("CP932", "b1", 0xFF71, "half-width katakana ｱ"),
    ("CP932", "5c", 0x005C, "backslash (ASCII, as in Windows/WHATWG, not JIS X 0201's yen sign)"),
    ("CP932", "7e", 0x007E, "tilde (ASCII, not JIS X 0201's overline)"),
]

# (encoding, input string, expected byte sequence (hex)) -- input direction (real PTY round-trip)
INPUT_ROUNDTRIP_CASES = [
    ("euc-jp-2007", "髙", "fce2"),
    ("euc-jp-2007", "①", "ada1"),
    ("euc-jp-2007", "鷗", "8fecbf"),
    ("CP932", "髙", "fbfc"),
    ("CP932", "①", "8740"),
    ("CP932", "日本", "93fa967b"),
]

# No case is left where input silently drops, so this is empty.
INPUT_DROP_CASES: list[tuple[str, str, str]] = []

# Fallback policy when conversion isn't possible.
# Output direction: bytes that can't make a character show U+FFFD for their
# first byte, and decoding goes on from the second, as in VS Code's editor
# (tests/test_editor_parity.py checks every such sequence).
# (encoding, input byte sequence (hex), fallback mode (always None now), expected text)
FALLBACK_OUTPUT_CASES = [
    ("euc-jp-2007", "a2af41", None, "\ufffd\ufffdA", "unassigned JIS X 0208 code: the second byte is read again, as a lead"),
    ("euc-jp-2007", "8fa141", None, "\ufffd\ufffdA", "JIS X 0212 sequence cut short by ASCII"),
    ("euc-jp-2007", "9b41", None, "\ufffdA", "0x9B is no CSI in EUC-JP"),
    ("CP932", "81ad", None, "\ufffd\uff6d", "unassigned pair: the second byte is read again (katakana)"),
    ("CP932", "8121", None, "\ufffd!", "a lead byte before ASCII isn't dropped"),
    # With upstream's identity fallback, an unmapped code silently
    # mis-converted into an unrelated character (Big5-HKSCS a180 -> U+A180).
    ("BIG5-HKSCS", "a180", None, "\ufffd\ufffd", "Big5-HKSCS unmapped; upstream mis-converts it to U+A180"),
    ("Big5", "8e40", None, "\ufffd@", "0x8E is a Big5 lead byte, not SS2"),
    ("GB18030", "81308141", None, "\ufffd0\u4e04", "GB18030 4-byte sequence broken at its last byte"),
    # gb18030_linear_to_codepoint upper-bound check regression: FE 39 FE 39
    # is byte-range-valid but its linear index (1587599) exceeds the
    # maximum (1237575, corresponding to U+10FFFF). Without the upper-bound
    # check, this would produce an invalid code point past U+10FFFF and get
    # output as invalid UTF-8.
    ("GB18030", "fe39fe39", None, "\ufffd", "GB18030 linear index exceeds upper bound"),
]

# Input direction: (encoding, input character, fallback mode, expected round-trip result)
# An unencodable character rejects the whole chunk of input it arrived in:
# substituting or dropping just that character would change the command the
# shell runs (`rm <emoji>*` used to become `rm ?*` / `rm *`). The user gets
# a bell (and, through -notify, a notification).
FALLBACK_INPUT_CASES = [
    ("euc-jp-2007", "A☃B", None, "\a", "unmapped character rejects the whole input (bell only)"),
    ("CP932", "A☃B", None, "\a", "CP932 unmapped character rejects the whole input (bell only)"),
    ("GBK", "A☃B", None, "\a", "OTHER charset path (other_ja.c) rejects too"),
]

# Input arriving in several reads after a rejection (a paste) is dropped
# until input pauses, so only the paste's tail can never reach the shell;
# the next input after the pause goes through. An open bracketed paste is
# still closed. (encoding, [(delay before write, text)], expected, description)
INPUT_REJECTION_SEQUENCE_CASES = [
    ("euc-jp-2007", [(0.0, "rm ☃"), (0.005, "*\n")], "\a", "the rest of a split paste is dropped too"),
    ("euc-jp-2007", [(0.0, "☃"), (0.3, "ok")], "\aok", "input after a pause goes through again"),
    ("euc-jp-2007", [(0.0, "\x1b[200~X"), (0.3, "☃\x1b[201~")], "^[[200~X\a^[[201~", "a rejected chunk still closes an open bracketed paste (the inner tty echoes ESC as ^[)"),
]

# Chinese, Korean, and single-byte encodings
# (encoding, input byte sequence (hex), expected code point, description)
MORE_OUTPUT_CASES = [
    ("GB18030", "95328337", 0x2000B, "GB18030 4-byte supplementary plane 𠀋"),
    ("GB18030", "90308130", 0x10000, "GB18030 4-byte supplementary plane boundary U+10000"),
    ("GB18030", "81308130", 0x0080, "GB18030 4-byte BMP gap U+0080"),
    ("GB18030", "baba", 0x6C49, "GB18030 2-byte part 汉 (regression check)"),
    ("GB18030", "e3329a35", 0x10FFFF, "GB18030 4-byte supplementary plane upper boundary U+10FFFF (gb18030_linear_to_codepoint upper-bound check regression)"),
    ("Big5", "a4a4", 0x4E2D, "Big5 中"),
    ("eucKR", "b0a1", 0xAC00, "EUC-KR 가"),
    ("GBK", "baba", 0x6C49, "GBK 汉 (newly added for the musl static build. It happened to work in the native glibc build via the iconv fallback, but failed in the static build)"),
    ("GB2312", "b0a1", 0x554A, "GB2312 阿 (same as above. Needed to construct source via the GL scheme (high-bit stripped))"),
    ("BIG5-HKSCS", "a4a4", 0x4E2D, "Big5-HKSCS 中 (same as above)"),
    # gen_tables.py lead-byte range bug regression check: lead bytes
    # 0xFD/0xFE also have real mappings (GBK/GB18030 2-byte part/Big5-HKSCS).
    ("GBK", "fe40", 0xFA0C, "GBK 0xFE lead byte 兀 (gen_tables.py lead-byte range bug regression)"),
    ("GB18030", "fe40", 0xFA0C, "GB18030 2-byte part 0xFE lead byte 兀 (same as above)"),
    ("BIG5-HKSCS", "fe40", 0x9442, "Big5-HKSCS 0xFE lead byte 鑂 (same as above)"),
    ("CP865", "a1", 0x00ED, "CP865 í (same as above)"),
    ("ISO8859-1", "e9", 0x00E9, "ISO-8859-1 é"),
    ("KOI8-R", "c1", 0x0430, "KOI8-R а"),
    ("CP437", "82", 0x00E9, "CP437 é"),
    ("CP852", "a1", 0x00ED, "CP852 í (table added by the fork)"),
    ("CP857", "a1", 0x00ED, "CP857 í (table added by the fork)"),
    ("CP1125", "a1", 0x0431, "CP1125 б (table added by the fork)"),
    ("MACROMAN", "a1", 0x00B0, "MACROMAN ° (table added by the fork)"),
    ("KOI8-T", "80", 0x049B, "KOI8-T қ"),
    ("KOI8-T", "d1", 0x044F, "KOI8-T я (Cyrillic half, same layout as KOI8-R)"),
]

# stack_gb18030()'s linear flag regression check: confirms an ASCII byte
# immediately after completing a 4-byte sequence doesn't get misresolved as
# a linear index into the BMP gap range and corrupted (encoding, input byte
# sequence (hex), expected output string, description).
GB18030_LINEAR_FLAG_CASES = [
    ("GB18030", "9532833741", "\U0002000B" + "A", "the ASCII 'A' right after a 4-byte supplementary-plane character isn't corrupted"),
    ("GB18030", "813081304142", "" + "AB", "the ASCII \"AB\" right after a 4-byte BMP-gap character isn't corrupted"),
]

# An invalid CP932 lead byte followed by a trail byte is two characters, the
# invalid byte shown as U+FFFD as in VS Code, never one 2-byte character.
# Before the fork's tables only counted rows they have, such a byte showed
# up as the Latin-1 character with its value.
# (encoding, input byte sequence (hex), expected output string, description)
CP932_INVALID_LEAD_BYTE_ARTIFACT_CASES = [
    ("CP932", "a040", "\ufffd@", "CP932 0xA0 (outside the half-width katakana range) + '@' isn't mistakenly merged into a single character"),
    ("CP932", "fd40", "\ufffd@", "CP932 0xFD (outside SJIS's valid lead-byte range) + '@' isn't mistakenly merged into a single character"),
    ("CP932", "fe40", "\ufffd@", "CP932 0xFE (outside SJIS's valid lead-byte range) + '@' isn't mistakenly merged into a single character"),
]

# iso2022.c T_128 control-range-drop regression check (a case the G3 (SS3)
# fix in copyIn() didn't cover). T_128 charsets (CP852, etc.) have byte-value
# assignments 0x80-0x9F whose n, after applying shift (0x80), can land in
# the control range 0x00-0x1F. Trying to type a character in this range via
# G3 (SS3) had reverse() itself succeed (i>=0), but fall through the GL
# range check (i>=0x20) without writing anything, and then continue without
# reaching fallback_policy — silently vanishing.
# (kg3_charset, input character, expected output string, description)
ISO2022_T128_CONTROL_RANGE_CASES = [
    ("CP 852", "ů", "\a", "CP852 byte 0x85 (a character absent from ISO8859-1, n=0x05 after shift) doesn't vanish silently via G3 (SS3): the input is rejected with a bell"),
]

# When several byte sequences decode to the same code point (CP932's
# NEC/IBM duplicates, Big5's duplicated box-drawing characters, ...), the
# input direction must send the bytes VS Code saves that code point as
# (iconv-lite, with converters.json's corrections), not whichever duplicate
# the reverse lookup happens to hit (see gen_tables.py's mark_decode_only()).
# (encoding, input text, expected hex sent to the child, description)
INPUT_CANONICAL_BYTES_CASES = [
    ("CP932", "￢ⅰ∵纊", "81cafa4081e6fa5c", "NEC/IBM duplicates encode like VS Code (not 0xEEF9/0xEEEF)"),
    ("euc-jp-2007", "￢∵", "a2cca2e8", "euc-jp-2007 duplicates encode like VS Code"),
    ("BIG5-HKSCS", "═", "f9f9", "Big5-HKSCS duplicated box drawing (0xA2A4/0xF9F9) encodes like VS Code"),
    ("KOI8-T", "қӯя", "80a1d1", "KOI8-T encodes through its table"),
    # The wave dash: 〜 (macOS's input methods) and ～ (Windows') are both
    # sent as the bytes shown as ～, and so is № in EUC-JP (as the
    # wave-dash-unify extension saves them; not iconv-lite's 0x8FA2B7/0x8FA2F1)
    ("euc-jp-2007", "〜～№", "a1c1a1c1ade2", "EUC-JP wave dash, fullwidth tilde and numero sign"),
    ("CP932", "〜～", "81608160", "Shift JIS wave dash and fullwidth tilde"),
    ("CP932", "\\¥~‾", "5c5c7e7e", "CP932 backslash/yen and tilde/overline both encode to 0x5C/0x7E (WHATWG)"),
]

# Regression check: confirms known-correct mappings and ASCII passthrough
# weren't broken by the fallback patches
FALLBACK_REGRESSION_CASES = [
    ("euc-jp-2007", "Hello, World!", "Hello, World!", "multiple ASCII characters"),
    ("CP932", "Hello, World!", "Hello, World!", "ASCII works under CP932 too"),
    ("euc-jp-2007", "髙鷗①〜", "髙鷗①～", "known Japanese characters (〜 comes back as ～, how 0xA1C1 is shown)"),
    ("CP932", "髙①〜", "髙①～", "known CP932 characters (〜 comes back as ～, how 0x8160 is shown)"),
    # The 3 encodings whose implementation the fork replaced. Patching the
    # shared functions once broke ASCII, so ASCII passthrough is always
    # verified.
    ("GBK", "Hello, World!", "Hello, World!", "ASCII under GBK (implementation replaced by the fork)"),
    ("BIG5-HKSCS", "Hello, World!", "Hello, World!", "ASCII under Big5-HKSCS (implementation replaced by the fork)"),
    ("CP865", "Hello, World!", "Hello, World!", "ASCII under CP865 (under the fork's fallback management)"),
    # Regression check for the T_128 control-range-drop fix (iso2022.c).
    # CP437/CP865's bytes 0x82/0xA1, after applying shift (0x80), land n
    # at 0x02 (control range) / 0x21 (GL range) respectively. Normal
    # encoding via G0 (GL) must not restrict reverse()'s lower bound (a
    # control-range result is legitimate there), so this had to be fixed
    # via the write-guard/continue symmetry on the iso2022.c side instead.
    # This confirms both boundaries (the control-range side and the
    # GL-range side) still round-trip correctly.
    ("CP437", "é", "é", "CP437 é (byte 0x82, n=0x02 after shift, control-range boundary)"),
    ("CP865", "í", "í", "CP865 í (byte 0xA1, n=0x21 after shift, GL-range boundary)"),
]


def run_output_case(enc: str, hexin: str, expect_cp: int | None) -> tuple[bool, str]:
    data = bytes.fromhex(hexin)
    p = subprocess.run([str(LUIT), "-c", "-encoding", enc], input=data, capture_output=True)
    try:
        text = p.stdout.decode("utf-8")
    except UnicodeDecodeError:
        return False, f"invalid UTF-8: {p.stdout!r}"
    if expect_cp is None:
        return True, f"(comparison skipped) got={p.stdout!r}"
    if len(text) != 1:
        return False, f"expected 1 character but got {len(text)}: {p.stdout!r}"
    got_cp = ord(text)
    if got_cp != expect_cp:
        return False, f"U+{got_cp:04X} != U+{expect_cp:04X}"
    return True, f"U+{got_cp:04X}"


def run_output_string_case(enc: str, hexin: str, expect_text: str) -> tuple[bool, str]:
    data = bytes.fromhex(hexin)
    p = subprocess.run([str(LUIT), "-c", "-encoding", enc], input=data, capture_output=True)
    try:
        text = p.stdout.decode("utf-8")
    except UnicodeDecodeError:
        return False, f"invalid UTF-8: {p.stdout!r}"
    if text != expect_text:
        return False, f"{text!r} != expected {expect_text!r}"
    return True, f"{text!r}"


def run_roundtrip_case(enc: str, text: str, expect_hex: str) -> tuple[bool, str]:
    nbytes = len(bytes.fromhex(expect_hex))
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(str(LUIT), ["luit", "-encoding", enc, "--", "head", "-c", str(nbytes)])
        os._exit(1)
    attrs = termios.tcgetattr(fd)
    attrs[3] = attrs[3] & ~termios.ECHO
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    time.sleep(0.4)
    os.write(fd, text.encode("utf-8"))
    out = b""
    end = time.time() + 1.5
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            continue
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if not d:
            break
        out += d
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except Exception:
        pass
    try:
        got_text = out.decode("utf-8")
    except UnicodeDecodeError:
        return False, f"invalid UTF-8: {out!r}"
    expect_text = bytes.fromhex(expect_hex)
    # After the round trip, it should come back as the original character in UTF-8
    if got_text != text:
        return False, f"after round trip {got_text!r} != input {text!r} (expected EUC/SJIS intermediate representation: {expect_hex})"
    return True, f"round trip OK ({expect_hex})"


def run_drop_case(enc: str, text: str, reason: str) -> tuple[bool, str]:
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(str(LUIT), ["luit", "-encoding", enc, "--", "head", "-c", "8"])
        os._exit(1)
    attrs = termios.tcgetattr(fd)
    attrs[3] = attrs[3] & ~termios.ECHO
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    time.sleep(0.4)
    os.write(fd, text.encode("utf-8"))
    out = b""
    end = time.time() + 1.0
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            continue
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if not d:
            break
        out += d
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    if out == b"":
        return True, f"silently vanished as expected ({reason})"
    return False, f"expected it to vanish but received: {out!r}"


def run_fallback_output_case(enc: str, hexin: str, mode: str | None, expect: str | None) -> tuple[bool, str]:
    args = [str(LUIT), "-c"]
    args += ["-encoding", enc]
    data = bytes.fromhex(hexin)
    p = subprocess.run(args, input=data, capture_output=True)
    if expect is None:
        if p.stdout == b"":
            return True, "0 bytes (as expected)"
        return False, f"expected 0 bytes but received: {p.stdout!r}"
    try:
        text = p.stdout.decode("utf-8")
    except UnicodeDecodeError:
        return False, f"invalid UTF-8: {p.stdout!r}"
    if text != expect:
        return False, f"expected {expect!r} but got {text!r}"
    return True, repr(text)


def run_fallback_input_case(enc: str, text: "str | list[tuple[float, str]]", mode: str | None, expect: str) -> tuple[bool, str]:
    """Launches luit on a raw-mode PTY and confirms via round trip the byte
    sequence the child (cat) actually received. The head -c approach was
    unstable due to shell startup timing in this environment, so this uses a
    subprocess approach where tty.setraw() fully raw-mode's the outer PTY
    instead (with ICANON left on, input
    got buffered until a newline, and unmapped-character fallback couldn't
    be verified correctly).
    """
    master, slave = pty.openpty()
    tty.setraw(master)
    args = [str(LUIT)]
    args += ["-encoding", enc, "--", "cat"]
    p = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=subprocess.PIPE, close_fds=True)
    os.close(slave)
    time.sleep(0.4)
    steps = text if isinstance(text, list) else [(0.0, text)]
    for delay, chunk in steps:
        time.sleep(delay)
        os.write(master, chunk.encode("utf-8"))
    out = b""
    end = time.time() + 1.2
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.3)
        if not r:
            continue
        try:
            d = os.read(master, 4096)
        except OSError:
            break
        if not d:
            break
        out += d
    try:
        os.close(master)
    except OSError:
        pass
    try:
        p.kill()
        p.wait(timeout=1)
    except Exception:
        pass
    try:
        got = out.decode("utf-8")
    except UnicodeDecodeError:
        return False, f"invalid UTF-8: {out!r}"
    if got != expect:
        return False, f"{got!r} != expected {expect!r}"
    return True, f"round trip OK ({got!r})"


def run_exit_status_case(command: str, expect: int) -> tuple[bool, str]:
    """luit exits with the child's status (128 + signal if it was killed),
    which is what VS Code reports as the terminal's exit code."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(LUIT), ["luit", "-encoding", "euc-jp-2007", "--", "sh", "-c", command])
    try:
        while os.read(fd, 1024):
            pass
    except OSError:
        pass
    got = os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1])
    os.close(fd)
    return got == expect, f"exit status {got} (expected {expect})"


def run_encode_last_arg_case(enc: str, command: str, expect_status: int, expect_output: str) -> tuple[bool, str]:
    """-encode-last-arg converts the last argument (a task's command line,
    which VS Code passes in UTF-8) as if it were typed: the shell gets it in
    the encoding, or, when it has a character the encoding can't represent,
    luit doesn't run it at all and fails."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(LUIT), ["luit", "-encoding", enc, "-encode-last-arg", "--", "sh", "-c", command])
    out = b""
    try:
        while True:
            chunk = os.read(fd, 1024)
            if not chunk:
                break
            out += chunk
    except OSError:
        pass
    got = os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1])
    os.close(fd)
    text = out.decode("utf-8", "replace")
    # No case's command line may run its "RAN" marker if it was refused
    # od's column spacing differs between GNU and BSD
    ok = got == expect_status and expect_output in " ".join(text.split()) and not (expect_status == 1 and "RAN" in text)
    return ok, f"exit status {got} (expected {expect_status}), output {text!r}"


# (encoding, command line, expected exit status, expected output substring)
ENCODE_LAST_ARG_CASES = [
    # od prints the bytes the shell received: 本 is CB DC in EUC-JP
    ("euc-jp-2007", "printf %s '本' | od -An -tx1", 0, "cb dc"),
    ("CP932", "printf %s '本' | od -An -tx1", 0, "96 7b"),
    ("KOI8-T", "printf %s 'қ' | od -An -tx1", 0, "80"),
    # Longer than one copyIn() chunk (BUFFER_SIZE), so multibyte characters
    # straddle chunk boundaries: 5000 x 本 must arrive as 10000 bytes
    ("euc-jp-2007", "printf %s 'a" + "本" * 5000 + "' | wc -c", 0, "10001"),
    # ASCII passes through, and the shell still parses the line
    ("euc-jp-2007", "echo plain && exit 4", 4, "plain"),
    # Not representable: nothing runs (no RAN), luit fails and names it
    ("euc-jp-2007", "echo RAN; echo '\U0001F600'", 1, "U+1F600"),
]


def run_notify_case() -> tuple[bool, str]:
    """-notify DIR: each rejected input is reported as one line,
    "unencodable <encoding> <pid> <hex code point>", to every Unix socket ("*.sock")
    in DIR (one per VS Code window; the window owning the terminal reacts)."""
    import socket
    with tempfile.TemporaryDirectory() as tmp:
        servers = []
        for name in ("a.sock", "b.sock"):
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(os.path.join(tmp, name))
            server.listen(1)
            server.settimeout(3)
            servers.append(server)
        pid, fd = pty.fork()
        if pid == 0:
            os.execv(str(LUIT), ["luit", "-encoding", "euc-jp-2007", "-notify", tmp, "--", "cat"])
        got = []
        try:
            time.sleep(0.4)
            os.write(fd, "☃".encode("utf-8"))
            for server in servers:
                conn, _ = server.accept()
                conn.settimeout(3)
                got.append(conn.recv(256))
                conn.close()
        except OSError as e:
            got.append(repr(e).encode())
        finally:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
            os.close(fd)
            for server in servers:
                server.close()
    expect = f"unencodable euc-jp-2007 {pid} 2603\n".encode()  # U+2603 ☃
    return got == [expect, expect], f"{got!r}"


# Started like a terminal starts a shell (pty.fork: a session leader with
# the pty as its controlling terminal), the process tree is inverted on Linux:
# the started process becomes the shell itself, so its status is the shell's,
# signals included. Elsewhere (macOS) luit stays the shell's parent and
# reports a signal as 128 + signal, as shells do.
EXIT_STATUS_CASES = [
    ("exit 0", 0),
    ("exit 3", 3),
    ("kill -TERM $$", -15 if sys.platform == "linux" else 128 + 15),
]


def run_inverted_tree_case() -> tuple[bool, str]:
    """The process a terminal starts ends up as the shell, and the converter
    isn't among its children (what VS Code checks before closing a terminal
    and uses for the working directory)."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(LUIT), ["luit", "-encoding", "euc-jp-2007", "--", "sh", "-c", "sleep 1"])
    try:
        time.sleep(0.5)
        exe = os.path.basename(os.readlink(f"/proc/{pid}/exe"))
        with open(f"/proc/{pid}/task/{pid}/children") as f:
            children = [int(c) for c in f.read().split()]
        child_exes = [os.path.basename(os.readlink(f"/proc/{c}/exe")) for c in children]
    finally:
        os.waitpid(pid, 0)
        os.close(fd)
    ok = exe not in ("luit",) and "luit" not in child_exes
    return ok, f"started process runs {exe}, its children: {child_exes}"


def run_quick_exit_case(runs: int = 2000) -> tuple[bool, str]:
    """A command that prints and exits at once still shows its output. The
    converter used to die with the shell's SIGHUP when it ran late (about 1
    in 300 runs with everything on one CPU), so this runs on one CPU, many
    times."""
    cpu = min(os.sched_getaffinity(0))
    lost = 0
    for _ in range(runs):
        pid, fd = pty.fork()
        if pid == 0:
            os.sched_setaffinity(0, {cpu})
            os.execv(str(LUIT), ["luit", "-encoding", "euc-jp-2007", "--",
                                 "sh", "-c", "printf 'out:\\306\\374\\n'; exit 3"])
        out = b""
        while True:
            r, _, _ = select.select([fd], [], [], 5)
            if not r:
                break
            try:
                data = os.read(fd, 4096)
            except OSError:
                break
            if not data:
                break
            out += data
        os.waitpid(pid, 0)
        os.close(fd)
        if "out:日".encode() not in out:
            lost += 1
    return lost == 0, f"output lost in {lost} of {runs} runs"


def run_slow_reader_case() -> tuple[bool, str]:
    """A paste isn't lost when the program reads it later than it arrives.
    luit's writes to the pty are non-blocking, and whatever didn't fit used
    to be dropped: with the reader a second late, 20 KB of 200 KB arrived."""
    import threading
    data = b"0123456789" * 20_000
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "received.bin")
        pid, fd = pty.fork()
        if pid == 0:
            os.execv(str(LUIT), ["luit", "-encoding", "ISO8859-1", "--", "sh", "-c",
                                 'stty raw -echo; sleep 1; exec cat > "$0"', out])
        time.sleep(0.5)

        def write_all() -> None:
            for i in range(0, len(data), 4096):
                os.write(fd, data[i:i + 4096])

        writer = threading.Thread(target=write_all, daemon=True)
        writer.start()
        size, last_change, start = -1, time.time(), time.time()
        while time.time() - start < 20:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    os.read(fd, 65536)
                except OSError:
                    break
            now = os.path.getsize(out) if os.path.exists(out) else 0
            if now != size:
                size, last_change = now, time.time()
            if now >= len(data) or (time.time() - start > 3 and time.time() - last_change > 2):
                break
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        writer.join(1)
        os.close(fd)
        got = open(out, "rb").read() if os.path.exists(out) else b""
    return got == data, f"{len(got)} of {len(data)} bytes arrived intact"


def run_layout_case() -> tuple[bool, str]:
    """Started the way a terminal starts a shell, luit inverts the tree where
    a session leader may give up its controlling terminal (Linux) and keeps
    the classic layout where it can't (TIOCNOTTY fails on macOS). Either
    way the shell runs and its exit status comes back, and whatever started
    luit (this process) isn't sent a signal."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(LUIT), ["luit", "-v", "-encoding", "euc-jp-2007", "--", "sh", "-c", "echo ran; exit 5"])
    out = b""
    try:
        while True:
            d = os.read(fd, 1024)
            if not d:
                break
            out += d
    except OSError:
        pass
    _, status = os.waitpid(pid, 0)
    os.close(fd)
    code = os.waitstatus_to_exitcode(status)
    note = next((l for l in out.splitlines() if b"classic layout" in l), b"inverted")
    classic = note != b"inverted"
    ok = code == 5 and b"ran" in out and classic == (sys.platform != "linux")
    return ok, f"exit status {code}, {note.decode(errors='replace').strip()}"


def run_resize_case() -> tuple[bool, str]:
    """With the inverted tree the converter owns the outer terminal, so a
    resize there (VS Code's) still reaches the shell's pty."""
    import fcntl
    import struct
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(LUIT), ["luit", "-encoding", "euc-jp-2007", "--", "sh", "-c", "sleep 0.8; stty size"])
    time.sleep(0.4)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 100, 0, 0))
    out = b""
    try:
        while True:
            d = os.read(fd, 1024)
            if not d:
                break
            out += d
    except OSError:
        pass
    os.waitpid(pid, 0)
    os.close(fd)
    return b"40 100" in out, f"{out!r}"


def luit_processes_running(marker: str) -> list[int]:
    """pids of running luit processes (by executable) whose command line
    contains marker"""
    found = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            if os.path.realpath(f"/proc/{entry}/exe") != str(LUIT.resolve()):
                continue
            with open(f"/proc/{entry}/cmdline", "rb") as f:
                if marker.encode() in f.read().replace(b"\0", b" "):
                    found.append(int(entry))
        except OSError:
            continue
    return found


def run_hangup_case() -> tuple[bool, str]:
    """Closing the outer terminal (VS Code disposing it) ends the shell and
    the converter, as for a regular terminal."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(LUIT), ["luit", "-encoding", "euc-jp-2007", "--", "sh", "-c", "sleep 30"])
    time.sleep(0.5)
    os.close(fd)
    deadline = time.time() + 3
    done = 0
    while time.time() < deadline and not done:
        done, _ = os.waitpid(pid, os.WNOHANG)
        time.sleep(0.05)
    if not done:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
        return False, "the shell was still running 3 seconds after the terminal closed"
    deadline = time.time() + 3
    while True:
        leftovers = luit_processes_running("sleep 30")
        if not leftovers or time.time() > deadline:
            break
        time.sleep(0.1)
    return not leftovers, f"shell exited; leftover converters: {leftovers}"


def run_title_case() -> tuple[bool, str]:
    """-title-suffix: luit renames itself (its command line, which VS Code
    reads for the tab name) after the inner foreground program plus the
    suffix, as output passes."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(str(LUIT), ["luit", "-encoding", "euc-jp-2007", "-title-suffix", "\u00a0(EUC-JP)",
                             "--", "sh", "-c", "echo start; exec sleep 2"])
    title = ""
    comm = ""
    rest = b""
    try:
        deadline = time.time() + 1.5
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                os.read(fd, 1024)
            for entry in os.listdir("/proc"):
                if not entry.isdigit():
                    continue
                try:
                    if os.path.realpath(f"/proc/{entry}/exe") != str(LUIT.resolve()):
                        continue
                    with open(f"/proc/{entry}/cmdline", "rb") as f:
                        parts = f.read().split(b"\0")
                    title = parts[0].decode()
                    rest = b" ".join(p for p in parts[1:] if p)
                    with open(f"/proc/{entry}/comm") as f:
                        comm = f.read().strip()
                except OSError:
                    continue
            if title.endswith("(EUC-JP)") and title.startswith("sleep"):
                break
    finally:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
        os.close(fd)
    # The title is argv[0] only; ps also shows what the process really is,
    # and its command name stays "luit" for pgrep/killall.
    ok = (title == "sleep\u00a0(EUC-JP)" and rest == b"[terminal-any-encoding]"
          and comm == "luit")
    return ok, f"title={title!r} rest={rest!r} comm={comm!r}"


def run_mark_copy_used_case() -> tuple[bool, str]:
    """Started by the extension (options in TERMINAL_ANY_ENCODING_ARGS, through
    the shell-named link), luit marks the directory of its copy as used, so
    the extension doesn't remove a copy that restored terminals still need."""
    with tempfile.TemporaryDirectory() as tmp:
        copy_dir = os.path.join(tmp, "0123456789abcdef")
        os.makedirs(os.path.join(copy_dir, "shims"))
        shutil.copy(LUIT, os.path.join(copy_dir, "luit"))
        os.symlink(os.path.join(copy_dir, "luit"), os.path.join(copy_dir, "shims", "bash"))
        os.utime(copy_dir, (0, 0))
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["TERMINAL_ANY_ENCODING_ARGS"] = "-encoding\neuc-jp-2007\n--\n/bin/true"
            os.execv(os.path.join(copy_dir, "shims", "bash"), [os.path.join(copy_dir, "shims", "bash")])
        try:
            while os.read(fd, 1024):
                pass
        except OSError:
            pass
        os.waitpid(pid, 0)
        os.close(fd)
        age = time.time() - os.stat(copy_dir).st_mtime
    return age < 60, f"directory last marked {age:.0f}s ago"


def run_classic_tree_case() -> tuple[bool, str]:
    """Without a controlling terminal of its own (not how terminals start
    shells) luit keeps the classic layout, shell as its child, and passes
    the shell's status on as an exit code (128 + signal)."""
    master, slave = pty.openpty()
    p = subprocess.Popen([str(LUIT), "-encoding", "euc-jp-2007", "--", "sh", "-c", "kill -TERM $$"],
                         stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    try:
        while os.read(master, 1024):
            pass
    except OSError:
        pass
    code = p.wait(timeout=5)
    os.close(master)
    return code == 128 + 15, f"exit status {code} (expected {128 + 15})"



def run_input_bytes_case(enc: str, text: str, expect_hex: str) -> tuple[bool, str]:
    """Types text into luit through a real PTY and checks the exact bytes the
    child receives (a round trip can't tell duplicates apart, since every
    candidate decodes back to the same character)."""
    nbytes = len(bytes.fromhex(expect_hex))
    with tempfile.TemporaryDirectory() as tmp:
        out_path = os.path.join(tmp, "received.bin")
        pid, fd = pty.fork()
        if pid == 0:
            os.execvp(str(LUIT), ["luit", "-encoding", enc, "--", "sh", "-c", f'head -c {nbytes} > "$0"', out_path])
            os._exit(1)
        attrs = termios.tcgetattr(fd)
        attrs[3] = attrs[3] & ~termios.ECHO
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        time.sleep(0.4)
        os.write(fd, text.encode("utf-8") + b"\n")
        deadline = time.time() + 2.0
        while time.time() < deadline:
            if os.path.exists(out_path) and os.path.getsize(out_path) >= nbytes:
                break
            time.sleep(0.05)
        time.sleep(0.1)
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(pid, 0)
        os.close(fd)
        got = open(out_path, "rb").read().hex() if os.path.exists(out_path) else ""
    if got != expect_hex:
        return False, f"sent {got or '(nothing)'} != expected {expect_hex}"
    return True, f"sent {got}"


def run_kg3_input_case(kg3_charset: str, text: str, expect: str) -> tuple[bool, str]:
    """Assigns a T_128 charset to G3 via -kg3, and confirms the byte sequence
    the child receives when typing a character absent from GL (G0)/G2 (SS2,
    default=ISO8859-1). -encoding is pinned to ISO8859-1 so G0/G2 are
    Latin-1-equivalent (assuming only characters convertible solely via
    G3's CP852, etc. are used).
    """
    master, slave = pty.openpty()
    tty.setraw(master)
    args = [str(LUIT), "-encoding", "ISO8859-1", "-kg3", kg3_charset, "--", "cat"]
    p = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=subprocess.PIPE, close_fds=True)
    os.close(slave)
    time.sleep(0.4)
    os.write(master, text.encode("utf-8"))
    out = b""
    end = time.time() + 1.2
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.3)
        if not r:
            continue
        try:
            d = os.read(master, 4096)
        except OSError:
            break
        if not d:
            break
        out += d
    try:
        os.close(master)
    except OSError:
        pass
    try:
        p.kill()
        p.wait(timeout=1)
    except Exception:
        pass
    try:
        got = out.decode("utf-8")
    except UnicodeDecodeError:
        return False, f"invalid UTF-8: {out!r}"
    if got != expect:
        return False, f"{got!r} != expected {expect!r}"
    return True, f"round trip OK ({got!r})"


def main() -> int:
    # Results as they happen, and a traceback if the process dies on a
    # signal: otherwise a crash loses everything still buffered.
    sys.stdout.reconfigure(line_buffering=True)
    faulthandler.enable()
    if not LUIT.exists():
        print(f"NG: {LUIT} not found. Run configure && make in transcoder/src first.", file=sys.stderr)
        return 1

    failures = 0
    total = 0

    print("== Output direction ==")
    for enc, hexin, expect_cp, desc in OUTPUT_CASES:
        total += 1
        ok, detail = run_output_case(enc, hexin, expect_cp)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {hexin} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== Input direction (real PTY round-trip) ==")
    for enc, text, expect_hex in INPUT_ROUNDTRIP_CASES:
        total += 1
        ok, detail = run_roundtrip_case(enc, text, expect_hex)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {text!r} -> {detail}")
        if not ok:
            failures += 1

    if INPUT_DROP_CASES:
        print("\n== Input direction (confirming silent drop on unencodable input) ==")
        for enc, text, reason in INPUT_DROP_CASES:
            total += 1
            ok, detail = run_drop_case(enc, text, reason)
            mark = "OK " if ok else "NG "
            print(f"{mark}[{enc}] {text!r} -> {detail}")
            if not ok:
                failures += 1

    print("\n== Chinese, Korean, and single-byte encodings (incl. GB18030 4-byte) ==")
    for enc, hexin, expect_cp, desc in MORE_OUTPUT_CASES:
        total += 1
        ok, detail = run_output_case(enc, hexin, expect_cp)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {hexin} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== GB18030 stack_gb18030() linear flag regression check ==")
    for enc, hexin, expect_text, desc in GB18030_LINEAR_FLAG_CASES:
        total += 1
        ok, detail = run_output_string_case(enc, hexin, expect_text)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {hexin} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== invalid CP932 lead bytes ==")
    for enc, hexin, expect_text, desc in CP932_INVALID_LEAD_BYTE_ARTIFACT_CASES:
        total += 1
        ok, detail = run_output_string_case(enc, hexin, expect_text)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {hexin} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== Input direction: duplicates encode to VS Code's bytes (real PTY) ==")
    for enc, text, expect_hex, desc in INPUT_CANONICAL_BYTES_CASES:
        total += 1
        ok, detail = run_input_bytes_case(enc, text, expect_hex)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {text!r} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== iso2022.c T_128 control-range-drop regression check (real PTY round-trip) ==")
    for kg3_charset, text, expect, desc in ISO2022_T128_CONTROL_RANGE_CASES:
        total += 1
        ok, detail = run_kg3_input_case(kg3_charset, text, expect)
        mark = "OK " if ok else "NG "
        print(f"{mark}[-kg3 {kg3_charset}] {text!r} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== Fallback: output direction ==")
    for enc, hexin, mode, expect, desc in FALLBACK_OUTPUT_CASES:
        total += 1
        ok, detail = run_fallback_output_case(enc, hexin, mode, expect)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc} fallback={mode or 'default'}] {hexin} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== Fallback: input direction (real PTY round-trip) ==")
    for enc, text, mode, expect, desc in FALLBACK_INPUT_CASES:
        total += 1
        ok, detail = run_fallback_input_case(enc, text, mode, expect)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc} fallback={mode or 'default'}] {text!r} -> {detail}  # {desc}")
        if not ok:
            failures += 1

    print("\n== input rejection across reads (real PTY round-trip) ==")
    for enc, steps, expect, desc in INPUT_REJECTION_SEQUENCE_CASES:
        total += 1
        ok, detail = run_fallback_input_case(enc, steps, None, expect)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {desc} -> {detail}")
        if not ok:
            failures += 1

    print("\n== Regression check for the fallback patches (real PTY round-trip) ==")
    for enc, text, expect, desc in FALLBACK_REGRESSION_CASES:
        total += 1
        ok, detail = run_fallback_input_case(enc, text, None, expect)
        mark = "OK " if ok else "NG "
        print(f"{mark}[{enc}] {desc} -> {detail}")
        if not ok:
            failures += 1

    print("\n== reporting rejected input (-notify) ==")
    total += 1
    ok, detail = run_notify_case()
    print(f"{'OK ' if ok else 'NG '}rejection reported to every socket -> {detail}")
    if not ok:
        failures += 1

    print("\n== process tree ==")
    # (name, function, needs Linux: /proc, or the Linux-only tab title)
    for name, fn, linux_only in [
            ("inverted (as started by a terminal)", run_inverted_tree_case, True),
            ("a command that prints and exits at once shows its output", run_quick_exit_case, True),
            ("inverted on Linux, classic elsewhere, shell runs either way", run_layout_case, False),
            ("a paste survives a program that reads it late", run_slow_reader_case, False),
            ("resize reaches the shell", run_resize_case, False),
            ("closing the terminal ends shell and converter", run_hangup_case, True),
            ("tab title follows the foreground program", run_title_case, True),
            ("started by the extension, marks its copy as used", run_mark_copy_used_case, False),
            ("classic (no controlling terminal)", run_classic_tree_case, False)]:
        if linux_only and not sys.platform.startswith("linux"):
            print(f"SKIP {name} (Linux only)")
            continue
        total += 1
        ok, detail = fn()
        print(f"{'OK ' if ok else 'NG '}{name} -> {detail}")
        if not ok:
            failures += 1

    print("\n== task command line (-encode-last-arg) ==")
    for enc, command, status, expect in ENCODE_LAST_ARG_CASES:
        total += 1
        ok, detail = run_encode_last_arg_case(enc, command, status, expect)
        mark = "OK " if ok else "NG "
        shown = command if len(command) <= 60 else command[:57] + "..."
        print(f"{mark}[{enc}] {shown!r} -> {detail[:200]}")
        if not ok:
            failures += 1

    print("\n== exit status ==")
    for command, expect in EXIT_STATUS_CASES:
        total += 1
        ok, detail = run_exit_status_case(command, expect)
        mark = "OK " if ok else "NG "
        print(f"{mark}sh -c {command!r} -> {detail}")
        if not ok:
            failures += 1

    print(f"\n{total - failures} of {total} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
