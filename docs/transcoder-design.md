# Transcoder design notes

Non-obvious design decisions, constraints, and known bugs behind **why the
code looks the way it does** in the transcoder
([a fork of luit](https://invisible-island.net/luit/), `transcoder/`). The
extension side is in [extension-design.md](extension-design.md).
Implementation change history and verification logs aren't kept here.

## Charset types and how `source` values are represented

`transcoder/vendor/luit-upstream/` is an unmodified copy of Thomas E.
Dickey's official distribution (tarball `luit.tar.gz`, version
`2.0-20250912`). `transcoder/src/` is the working copy, carrying the fork's
own patches and additional files (`builtin_ja.c`/`other_ja.c`, etc.).

When adding a new conversion table, how `source` values are represented
depends on luit's internal charset type. Mapping confirmed by measurement:

| Charset type           | Example                                  | `source` value representation                 |
| ---------------------- | ---------------------------------------- | --------------------------------------------- |
| T_94192 (shift=0x8000) | Big5                                     | `n+shift` matches the raw 2-byte value        |
| T_9494 (shift=0)       | Japanese G1, GB2312, EUC-KR              | GL scheme (high bit stripped from both bytes) |
| OTHER charset          | CP932, GBK, Big5-HKSCS, GB18030 (2-byte) | Raw 2-byte value                              |
| T_128                  | Single-byte sets like CP852              | Simple 256-entry table, shift applied         |

The builtin table mechanism itself (`BuiltInMapping.source`/`.target` are
`unsigned`) was originally designed to support multi-byte character sets, but
`luitLookupMapping()`'s `case umBUILTIN:` hardcoded the `size` argument to
`initLuitConv()` as `us8BIT`, so only single-byte tables could actually be
used. Fixing the one spot that passes `size` through as-is was enough to
enable it.

CP932 can't reuse SJIS's coordinate conversion formula (`mapping_sjis()`): it
only works correctly within the standard 94-row range (the main JIS X 0208
table), and feeding it the IBM-extension lead bytes (`0xFA`-`0xFC`) produces
an invalid row number and mis-converts (stock luit's `SJIS` still does). So a
dedicated lookup table keyed directly on the raw SJIS 2-byte value
(`cp932-direct-0`) was added instead.

CP932's `0x5C`/`0x7E` decode as ASCII `\`/`~`, as on Windows, in the WHATWG
Encoding Standard and in VS Code's editor (iconv-lite), not as JIS X 0201's
`¥`/`‾` like upstream's `SJIS`. Otherwise paths and `~/` would read
differently from the editor, and copied output would contain U+00A5. Typing
`¥`/`‾` still sends `0x5C`/`0x7E`, like the WHATWG encoder.

## Policy: ICU as the source for conversion tables

Conversion tables are generated from ICU (`uconv`). Two reasons: data
derived from glibc can't be embedded due to its license (LGPL), and using
ICU gives reproducibility that a golden hash can detect if the ICU version
changes. `tools/gen-tables/gen_tables.py` generates
`transcoder/src/builtin_ja.c` from the declarations in `converters.json`
(table name, ICU converter name, plane type), and `--check` verifies that
regenerating against the current ICU environment matches
`tools/gen-tables/golden/tables.sha256`. The generated `builtin_ja.c`/
`other_ja.c` are not hand-edited.

The built-in tables are looked up before anything else (`lookup_order` in
`luit.c`). Upstream looks in X11's font encodings (`.enc` files listed in an
`encodings.dir` found at build time, or named by `FONT_ENCODINGS_DIRECTORY`)
first, so wherever those are installed (Debian/Ubuntu's `xfonts-encodings`)
`gbk-0` and `big5hkscs-0` decoded with that data instead of ours, and the
fallback no longer applied. CI's native test job installs `xfonts-encodings`
to keep that covered.

The one exception is KOI8-T, which ICU has no converter for. Its table
(`"iconv_lite"` in `converters.json`) comes from iconv-lite, the library
VS Code itself decodes files with, so the terminal agrees with the editor.
Both reasons above still hold: iconv-lite is MIT-licensed, and its version
is pinned by `package-lock.json` (a devDependency), so a change shows up in
the golden hash like an ICU change does. To keep "only verified conversions"
true, generation fails unless the table matches Python's `koi8_t` codec and
glibc's KOI8-T byte for byte; those are only compared against, never
embedded. iconv-lite tables are single-byte only, and generation also fails
if two bytes decode to the same character, since deciding which one encodes
would need ICU (`mark_decode_only()`).

ICU and Python's standard `gb18030` codec differ by standard version (a
difference was observed near U+1E3F); ICU is used, per the policy above.
GB18030's supplementary planes (U+10000 and above) are expressed
with a single formula (`linear = cp - 0x10000 + 189000`), while the BMP gaps
are expressed via a table of 210 contiguous ranges extracted by measurement
from ICU (`gb18030_ranges.c`, auto-generated).

## Fallback design for conversion failures

On output (decoding), an unmapped byte sequence becomes `U+FFFD`, like
VS Code's editor shows undecodable bytes. There's no option to skip or
escape it instead: silently dropped output helps nobody, and such a setting
would be luit's vocabulary leaking into the UI.

On input (encoding) there's no option: a chunk of keyboard input containing a
character the encoding can't represent is rejected as a whole, and the user
gets a bell. Substituting `?` or dropping just that character would change the
command the shell runs (`rm <emoji>*` becomes `rm ?*` / `rm *`). `copyIn()`
converts each read into a buffer and only writes it if every character
converted. A paste can arrive in several reads, so input is also dropped until
it pauses for 50 ms after a rejection (forwarding only the tail of a paste
could run a different command), and if a bracketed paste (`ESC [200~`) was
already forwarded, its end marker is still passed through so the shell doesn't
stay in paste mode. Warning on stderr instead isn't an option either: the
messages would be mixed into the terminal output.

The bell alone is silent with VS Code's default settings (the terminal bell
signal only sounds with a screen reader, and the visual bell is off), so a
rejected paste would just vanish. luit's `-notify <dir>` reports each
rejection as one line, `unencodable <encoding> <pid> <hex code point>`
(the first character that couldn't be encoded, which the notification
names), to every Unix socket
(`*.sock`) in a per-user directory where each VS Code window's extension
listens on its own socket. The pid is the one VS Code knows the terminal by
(`Terminal.processId`), so only the window owning the terminal shows the
notification, at most every 5 seconds per encoding. Broadcasting to a fixed
directory, rather than to one window's socket, keeps it working for
terminals revived after a window reload, whose extension host is new.
Sockets are connected per report (after the child was forked, close-on-exec,
non-blocking, no SIGPIPE) and skipped if they don't accept right away. The
directory's name is predictable, so it's only used if it's a real directory
owned by the user and closed to others.

When luit keeps the classic layout (shell as its child; see "Process tree"
below) it exits with the child's exit status (128 + signal if it was
killed), so VS Code's "terminal process exited with code N" alert reflects
the shell, not always 0. With the inverted layout the process VS Code
started is the shell, so its status is the shell's to begin with.

**Design principle: never patch the low-level functions shared by all
charsets regardless of ISO2022/OTHER (`luitMapCodeValue()`/`luitReverse()`)
directly.** These are called from places whose blast radius is hard to fully
map, including startup-time locale probing, and patching them directly causes
serious regressions, even breaking plain ASCII passthrough (this actually
happened once and had to be reverted). Instead, the design is:

1. Keep `luitMapCodeValue`/`luitReverse` completely stock.
2. Add new pure reference functions, `luitMapCodeValueFound()`/
   `luitReverseFound()`, that only report whether a mapping was found,
   without any fallback.
3. Use these new functions only from the call sites of charsets added by the
   fork (`FontencCharsetRecode`/`FontencCharsetReverse` in `charset.c`,
   `mapping_cp932`/`reverse_cp932` in `other_ja.c`, etc.).

Stock upstream charsets (ones the fork hasn't replaced the tables for) are
out of scope for this policy and keep their old identity-fallback behavior.

## Override mechanism for differential entries like the wave dash

In `initializeBuiltInTable()`'s implementation, when multiple entries share
the same `source`, the **decode direction** (`table_utf8[j]`) is won
(overwritten) by whichever entry is processed last in the array, while the
**encode direction** (`rev_index`) unconditionally appends every entry
(duplicates allowed). This asymmetry is used to implement "decoding is
overridden, encoding accepts both" just by appending an override row after
the base row. luit itself is untouched. Each table in `converters.json` can
have an `overrides` array, and `gen_tables.py` appends the override rows
after the base rows (from ICU). Example: EUC `A1C1` → U+301C (wave dash) for
`jisx0208-2007-0`.

## Duplicate code points: encode the way ICU does

Several tables have more than one source code decoding to the same code
point (CP932's NEC row 13 / NEC-selected IBM / IBM extensions, e.g. U+FFE2 at
`0x81CA`/`0xEEF9`/`0xFA54`; Big5's duplicated box-drawing characters). Every
row lands in `rev_index`, and `bsearch()` over equal keys returns an
unspecified one, so input could send a nonstandard sequence (`0xEEF9`), and
the choice could even differ between glibc and musl.

`gen_tables.py`'s `mark_decode_only()` asks ICU which bytes it encodes each
duplicated code point to, and writes the other rows as
`DECODE_ONLY(ucs)` (the `BUILTIN_DECODE_ONLY` bit in
`BuiltInMapping.target`). `initializeBuiltInTable()` decodes those rows as
usual but leaves them out of `rev_index`, so encoding always picks ICU's
choice. No single rule (lowest/highest code) matches ICU across CP932, so
this has to be data-driven. Upstream tables never set the bit (Unicode stops
at 0x10FFFF). `tests/test_encodings.py` checks the exact bytes sent.

The table probes run `uconv` with `--callback substitute`, not `skip`. With
`skip`, an invalid byte in a probed pair silently disappeared, so a 1-byte
character plus an invalid byte (CP932 `0xD8 0x80`, Big5 `0x81 0xFF`) came
back as one character and was recorded as a bogus 2-byte mapping.

## Differences surfaced by the musl static build

In the musl static build used for distribution, encodings that worked
correctly in the native (glibc dynamic-linked) build (GBK/GB2312/Big5/
Big5-HKSCS/EUC-KR/CP865) all failed across the board. The cause: they were
only working by accident via glibc's iconv fallback path (`umICONV`), which
isn't available under static linking. "Verified in the native build" does
not guarantee "works in the binary actually distributed," so verification is
always done against the musl static build binary.

## Deliberately unsupported

- **EUC-JP-MS variant**: ICU has no matching converter (the closest one
  differs in the position of ①/髙 and can't handle JIS X 0212 either). Real
  hardware verification against the target system confirmed 髙 is at `FC E2`
  (= the default `euc-jp-2007`), so this is covered in practice.
- **Plain EUC-JP variant (without extensions)**: skipped because deciding the
  boundaries of the NEC/IBM extension rows without a primary source carries
  too much risk. This is for the rare case where being unable to use ①/髙 is
  acceptable by spec, and is low priority.

## Process tree: the shell is the process VS Code started

VS Code (like terminals in general) looks at the process it started: its
children decide `confirmOnKill`/`confirmOnExit` ("has running processes";
the ignore list is empty on Linux/macOS), its working directory is used when
there's no shell integration, and its exit status is the terminal's. With
luit in between, the shell itself always counted as a running child, and
the working directory and exit status were luit's.

When started the way terminals start shells, as a session leader whose
controlling terminal is its stdin (`canInvert()`), luit inverts the tree
(`condomInverted()` in `luit.c`):

1. It forks the converter and detaches it (double fork, so it's not a child
   of the shell); the converter calls `setsid()` to leave the process group.
2. It gives up the outer terminal (`TIOCNOTTY`, ignoring the SIGHUP this
   sends its own group) and tells the converter, which takes the outer
   terminal as its controlling terminal (`TIOCSCTTY`). The converter then
   gets SIGWINCH when VS Code resizes the terminal and SIGHUP when it closes,
   as luit always did.
3. It takes the inner pty as controlling terminal and execs the shell, so the
   pid VS Code knows is the shell's. Rejection reports carry that pid.

The converter exits when the inner pty closes, i.e. once the shell and
everything it started are gone. Anything else (no controlling terminal, or
`-p`) keeps the classic layout. Tests check the tree, resizing, closing the
outer terminal and the exit status, on glibc and musl builds; macOS isn't
covered by CI (its `TIOCNOTTY`/`TIOCSCTTY` semantics are the same on paper).

## Known upstream bugs and fixes

- `allocatePty()` in `sys.c`, `openpty()` path: `openpty()` also opens the
  slave side, and only the child closed it (in `openTty()`). The parent
  kept it open, so the master never saw EOF/EIO when the shell exited and
  luit never exited: the terminal stayed open after `exit`. Only builds
  taking this path are affected, which includes the musl static build that
  is distributed (the glibc build uses `posix_openpt()`), so it went
  unnoticed until a test checked the exit status against the musl binary.
  The parent now closes its copy right after forking (`closeParentTty()`).

- `copyIn()` in `iso2022.c`: the G3 (SS3) block unconditionally `continue`d
  even when the reverse lookup failed (asymmetric with the matching G2
  block). With encodings where `IF_SS` is active (EUC-family encodings in
  general), characters absent from G1/G2/G3 couldn't reach the fallback
  path. Fixed it to match G2's symmetric behavior. The same kind of bug
  existed on the CP932 side too (via `OTHER`).
- Even after that fix, the T_128 charset (CP852 etc., `shift`=0x80) still had
  the same symptom (silent disappearance) via a different code path: because
  some source bytes map, after applying shift, into the control range
  (0x00-0x1F), `reverse()` itself succeeds, but the write falls through the
  SS2/SS3 write guard (`i>=0x20`) and nothing gets written, then `continue`s
  anyway. Unconditionally imposing a lower bound on T_128 would be a
  regression that breaks legitimate GR/LS-path behavior (the intentional
  acceptance of C1 characters when T_128 is assigned to GR), so the fix only
  `continue`s when a write actually happened.
- `stack_gb18030()` (an upstream file reused unmodified): the `linear` flag
  set on completing a 4-byte sequence wasn't reset on the ASCII passthrough
  path. An ASCII byte immediately following a 4-byte GB18030 character could
  be misinterpreted as a linear index, causing data corruption where it
  turned into an unrelated mojibake character. Fixed by explicitly resetting
  `linear = 0` on the ASCII passthrough path (the first fork-local change to
  an upstream-derived file).
- `gb18030_linear_to_codepoint()`: the supplementary-plane check
  (`linear >= 189000`) had no upper bound, so an invalid 4-byte sequence that
  was byte-range-valid but had a linear index past the maximum could produce
  an invalid code point beyond U+10FFFF. Added an upper bound.
- `gen_plane_raw2byte()` in `tools/gen-tables/gen_tables.py`: the lead-byte
  scan range was `0x81`-`0xFC`, omitting the valid lead bytes `0xFD`/`0xFE`
  used by GBK/GB18030 (2-byte part)/Big5-HKSCS from what got generated.
  Simply widening the range had a side effect: it let in spurious 2-byte
  entries where an invalid lead byte, considered alone, happened to decode
  identically to the trail byte decoded on its own across every entry. So a
  heuristic was added: for each candidate lead byte, compare against decoding
  the trail byte alone, and treat a lead byte as an artifact (and exclude it)
  if every entry matches.
