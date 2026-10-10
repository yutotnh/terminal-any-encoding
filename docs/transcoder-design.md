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
own patches and additional files (`builtin_fork.c`/`other_fork.c`, etc.).

How a table's `source` values are represented depends on luit's internal
charset type (`plane` in `converters.json`):

| Charset type               | Example                                                        | `source` value representation                 |
| -------------------------- | -------------------------------------------------------------- | --------------------------------------------- |
| OTHER charset, EUC-JP      | JIS X 0208 (`0xA1A1`-) and JIS X 0212 (after `0x8F`)           | GL scheme (high bit stripped from both bytes) |
| OTHER charset, EUC-JP      | JIS X 0201 katakana (after `0x8E`)                             | The byte                                      |
| OTHER charset              | CP932, GBK, GB 2312, CP949 (EUC-KR), Big5, Big5-HKSCS, GB18030 | The raw byte or 2-byte value                  |
| T_128 / T_96 (single-byte) | CP852, Windows-1252, ISO 8859-x, KOI8-x                        | The byte, all 256 of them                     |

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
(`cp932-direct-0`) was added instead. GBK, GB 2312, CP949 and Big5-HKSCS
work the same way, through shared helpers in `other_fork.c`.

A table "finds" a code only if it has a row for it
(`luitMapCodeValueFound()` checks the row's text): upstream presets the
first `length` codes of every table to themselves, so an unmapped byte such
as CP932's `0xA0` showed up as the Latin-1 character instead of U+FFFD.

## Policy: iconv-lite as the source for conversion tables

The terminal shows what VS Code's editor shows: the same bytes are the same
characters in both, and text copied from one can be searched for in the
other. So every table is generated from iconv-lite, the library the editor
reads and saves files with, in the package and version VS Code ships
(`@vscode/iconv-lite-umd`, pinned in `package-lock.json`).
`tools/gen-tables/gen_tables.py` decodes every byte sequence of each table's
shape on its own (`tools/gen-tables/iconv_lite.js`) and writes
`transcoder/src/builtin_fork.c` and `gb18030_ranges.c` from the declarations
in `converters.json`; `--check` verifies that regenerating matches
`tools/gen-tables/golden/tables.sha256`. The generated files are not
hand-edited. iconv-lite is MIT-licensed; THIRD-PARTY-NOTICES.md says where
its own data comes from.

`tests/test_editor_parity.py` decodes every byte sequence the editor
decodes as one character (1-, 2-, 3- and GB18030's 4-byte ones) and types
every character the editor can save, through a real PTY, for every encoding,
and compares with iconv-lite. The intended differences, and only those:

- **Typed text comes back as the bytes it was shown from.** iconv-lite's
  EUC-JP encoder doesn't do that for two characters: ～ U+FF5E (shown from
  `0xA1C1`) is saved as `0x8FA2B7`, and № U+2116 (from `0xADE2`) as
  `0x8FA2F1`, which other tools can't read (VS Code issue #48802, iconv-lite
  #145; the wave-dash-unify extension corrects it on save). They're sent as
  `0xA1C1` and `0xADE2` (`encode_corrections` in `converters.json`). 〜
  U+301C, which iconv-lite can't encode at all, is sent as the wave dash,
  EUC-JP `0xA1C1` or Shift JIS `0x8160` (an input alias, below): macOS's input
  methods type the wave dash as U+301C, Windows' as U+FF5E, and the editor
  shows those bytes as U+FF5E. So the terminal behaves like the editor with
  wave-dash-unify installed.
- **C1 controls** (U+0080-U+009F) aren't compared: in ISO 8859 terminals
  `0x8E`/`0x8F`/`0x9B` are SS2/SS3/CSI, not text.
- **GB18030's unassigned 4-byte ranges** (linear index 39420-188999, and past
  U+10FFFF) are invalid in the WHATWG Encoding Standard, but iconv-lite
  decodes them anyway (`0x8431A530` as U+10000); luit shows U+FFFD.

As in VS Code, EUC-KR is read as CP949 (EUC-KR plus Unified Hangul Code) and
GB 2312 with iconv-lite's `gb2312` table (GBK without its user-defined
areas); the extension still passes `eucKR`/`GB2312`, so the locale chosen
for them doesn't change.

Only the built-in tables are used (`lookup_order` in `luit.c`). Upstream
also looks in X11's font encodings (`.enc` files listed in an
`encodings.dir` found at build time, or named by
`FONT_ENCODINGS_DIRECTORY`), the C library's iconv, and finally takes bytes
as code points, all of which depend on the machine: with Debian/Ubuntu's
`xfonts-encodings` installed, `gbk-0` and `big5hkscs-0` decoded with that
data instead of ours, and musl's iconv, which the distributed binaries have,
has no CP1253/1254/1256/1257/1258/874 at all (luit fell back to ISO 8859-1
for them). Every charset a supported encoding uses has a generated table;
the ones named like upstream's (`iso8859-*`, `koi8-*`) replace them, since
`findBuiltinEncoding()` looks in `builtin_fork.c` first. CI's native test job
installs `xfonts-encodings`.

GB18030's supplementary planes (U+10000 and above) are a single formula
(`linear = cp - 0x10000 + 189000`), and the BMP's 4-byte sequences a table
of contiguous linear-index ranges (`gb18030_ranges.c`), both checked by the
parity test.

## Fallback design for conversion failures

On output (decoding), an unmapped byte sequence becomes `U+FFFD`, like
VS Code's editor shows undecodable bytes. There's no option to skip or
escape it instead: silently dropped output helps nobody, and such a setting
would be luit's vocabulary leaking into the UI.

Where the bytes stop making a character is decided as in the editor
(iconv-lite): the first byte shows `U+FFFD` and decoding goes on from the
second, whatever byte broke the sequence and however long it was. EUC-JP
`0x8F 0xA1 0x41` is `U+FFFD U+FFFD A`, GB18030 `0x81 0x30 0x81 0x41` is
`U+FFFD 0 丄`. Every multi-byte encoding is an OTHER charset for this: its
stack function only assembles bytes (any second byte makes a 2-byte code,
unmapped if invalid) or returns `OTHER_INVALID`, and `copyOut()`'s
`otherByte()` keeps the bytes it held, shows `U+FFFD` and reads the rest
again. An unmapped 4-byte GB18030 sequence stays one `U+FFFD` (the
WHATWG Encoding Standard's choice; iconv-lite shows unrelated characters
there, see above). A lone lead byte waits for the next one: a pty never ends
the output it's in the middle of.

On input (encoding) there's no option: a chunk of keyboard input containing a
character the encoding can't represent is rejected as a whole, and the user
gets a bell. Substituting `?` or dropping just that character would change the
command the shell runs (`rm <emoji>*` becomes `rm ?*` / `rm *`). `copyIn()`
converts each read into a buffer and only writes it if every character
converted. Warning on stderr instead isn't an option either: the messages
would be mixed into the terminal output.

A paste can arrive in several reads, and forwarding only its tail could run
a different command, so what follows a rejection is dropped too:

- Inside a bracketed paste (`ESC [200~` ... `ESC [201~`), up to its end
  marker, however late the rest arrives.
- Otherwise until input pauses for 50 ms.
- Either way for at most 2 s from the rejection, not extended by the input
  it drops. luit can't know that an end marker will come (VS Code's
  `terminal.integrated.ignoreBracketedPasteMode`, a terminal reset, a lost
  connection), so without the bound a wrong guess would drop every later
  keystroke. A rest arriving later gets through: luit can't tell it from
  typing. Locally that doesn't happen: VS Code writes a paste to the pty at
  once (on Linux, 300 KB arrived within 3 ms), and luit reads what it drops
  without waiting for the shell. The clock stops while luit waits for a
  busy program to take what it converted before, so a rest waiting behind
  that isn't late (`tests/paste_driver.c` checks it with a pty that takes
  nothing for longer than the bound).

Dropping up to the end marker needs bracketed paste on, which luit follows
in the program's output (`ESC [?2004h`, also combined as in
`ESC [?1049;2004h`, and `ESC [?2004l` or a reset, `ESC c`, to turn it off).

The paste markers in input aren't passed on as they come. luit holds what
may be one (`ESC [ 2 0 0`, so far) until it is one or isn't, and then
passes on a whole marker by these rules, which `tests/paste_driver.c`
checks for every way a rejected paste can be split into reads (cut in
either marker, before the rejected character, with gaps around the pause
and the bound), for rejected reads with other escapes in them, and for
every way input with nothing rejected can be cut into two or three reads,
with the time faked:

- The shell never gets part of a marker: it gets a start marker unless it's
  dropped, and an end marker unless the paste's start was (the end of a
  paste whose start went through as text, cut for 10 ms after its ESC or for
  2 s later, goes through too: the shell may have taken that start for one).
  So a start marker goes through even when what follows it is rejected, and
  the shell gets an empty paste.
- With nothing rejected, the shell gets the same bytes however the input
  is cut into reads and however long between them: those of the whole
  input converted at once, as a task's command line is.
- A shell in a paste gets its end marker exactly once, when it comes,
  dropped or not.
- What comes after the drop goes through, including the rest of a paste
  after the bound.
- A read is handled in parts between markers, so input after a paste's end
  in the same read is converted on its own: a character there that can't
  be encoded is rejected by itself (the notification still names the first
  one). A part is rejected as a whole, other escapes in it (keys such as
  `ESC [D`) included, and so is what's held at the end of a rejected read
  if it doesn't turn out to be a marker. Input after a marker is converted
  as if the marker had gone through the converter, which ends an escape
  left open before it (an Escape key right before a paste).

A lone ESC is what the Escape key sends, so one held at the end of a read
goes on as a key after 10 ms if nothing follows; more of a marker (`ESC [`
...) is held for 2 s at most, as no key sends it alone. Inside a paste
what's held waits for what comes next (in a paste the shell didn't get the
start of, for 2 s at most): it's most likely the start of the end marker,
which the terminal always sends, and passing it on as text would leave the
shell in the paste. An ESC that nothing follows for 10 ms outside a paste is
taken for a key, so a start marker split right after its ESC by that long
isn't one: luit can't tell the two apart. If the rest of that paste is then
rejected, the shell gets the ESC and the end marker only. A start marker
split for longer than 2 s goes through as text too, and the shell may take
it with what comes later for one; if the rest of that paste is rejected, its
end marker is dropped with it, and the shell can stay in the paste. Locally
neither happens, as VS Code writes a paste to the pty at once. A task's
command line (`-encode-last-arg`) isn't keyboard input: it's converted
without any of this, its escapes and markers passed on as they are, and
leaves the paste state alone.

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
   and every `mapping_*`/`reverse_*` in `other_fork.c`). The `other_fork.c`
   ones return `U+FFFD` for a code without a character, and 0 for a
   character without a code, which `copyIn()` rejects (0 is also what
   upstream's `reverse_gb18030()` returns when it finds nothing).

Stock upstream charsets (ones the fork hasn't replaced the tables for) are
out of scope for this policy and keep their old identity-fallback behavior.

## Input aliases: a character that's only typed

In `initializeBuiltInTable()`'s implementation, when multiple entries share
the same `source`, the **decode direction** (`table_utf8[j]`) is won by
whichever entry is processed last in the array, while every entry can land
in the reverse index. Each table in `converters.json` can have an
`input_aliases` array, and `gen_tables.py` puts those rows before the base
rows: the base row still decides what's displayed, and the alias only adds a
character that's sent as those bytes, e.g. U+301C → EUC `A1C1` in
`jisx0208-2007-0`. luit's only change for this is in
`initializeBuiltInTable()`, which frees the earlier row's text when a later
row for the same `source` replaces it (upstream's tables have one row per
`source`, so it never needed to). Whether a row is used for encoding is
decided like for any other row (next section).

## Which bytes a character is sent as

Several characters have more than one byte sequence (CP932's NEC row 13 /
NEC-selected IBM / IBM extensions, e.g. U+FFE2 at `0x81CA`/`0xEEF9`/
`0xFA54`; EUC-JP's IBM extension kanji in both JIS X 0208's rows 89-92 and
JIS X 0212; Big5's duplicated box-drawing characters). Every row would land
in `rev_index`, `bsearch()` over equal keys returns an unspecified one, and
luit tries JIS X 0208 before JIS X 0212, so input could send a sequence the
editor never writes.

`gen_tables.py`'s `mark_decode_only()` therefore lets a row encode only if
its bytes are what its character is sent as: iconv-lite's choice, unless
`encode_corrections` overrides it. The other rows are written as
`DECODE_ONLY(ucs)` (the `BUILTIN_DECODE_ONLY` bit in
`BuiltInMapping.target`), which `initializeBuiltInTable()` decodes as usual
but leaves out of `rev_index`. This also covers duplicates across tables
(EUC-JP's IBM extension kanji are sent as JIS X 0212, so their JIS X 0208
rows are decode-only) and characters the editor can't save at all. With the
two EUC-JP corrections, every EUC-JP character with several byte sequences
is sent exactly as glibc's EUC-JP-MS sends it; CP932's 396 duplicated
characters are sent as Windows (glibc's CP932) sends them. Upstream tables
never set the bit (Unicode stops at 0x10FFFF).

## Differences surfaced by the musl static build

In the musl static build used for distribution, encodings that worked
correctly in the native (glibc dynamic-linked) build (GBK/GB2312/Big5/
Big5-HKSCS/EUC-KR/CP865) all failed across the board. The cause: they were
only working by accident via glibc's iconv fallback path (`umICONV`), which
isn't available under static linking. "Verified in the native build" does
not guarantee "works in the binary actually distributed," so verification is
always done against the musl static build binary.

## Deliberately unsupported

- **Encodings VS Code doesn't offer**, such as the EUC-JP-MS and plain
  EUC-JP variants: the terminal offers the editor's list, so both can be
  used on the same files. EUC-JP input already sends what EUC-JP-MS sends for
  every character with several byte sequences.

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

1. It gives up the outer terminal (`TIOCNOTTY`, ignoring the SIGHUP this
   sends its own process group, i.e. itself; `releaseOuterTerminal()`). If
   that fails, as it does on macOS (`ENOTTY`), nothing has
   changed yet and luit keeps the classic layout (`-v` says why).
2. It forks the converter and detaches it (double fork, so it's not a child
   of the shell). The helper in between calls `setsid()` first, so the
   converter is never in the shell's process group: when the shell exits it
   sends that group SIGHUP, and a converter that hadn't left it yet (it can
   run late on a busy machine) died before reading anything, so a command
   that printed and exited at once showed nothing. The converter calls
   `setsid()` again and takes the outer terminal as its controlling terminal
   (`TIOCSCTTY`), so it gets SIGWINCH when VS Code resizes the terminal and
   SIGHUP when it closes, as luit always did.
3. It takes the inner pty as controlling terminal and execs the shell, so the
   pid VS Code knows is the shell's. Rejection reports carry that pid.

The converter exits when the inner pty closes, i.e. once the shell and
everything it started are gone. Anything else (no controlling terminal, or
`-p`) keeps the classic layout. Upstream's "child failed" `SIGABRT` to the
parent is only sent in the classic layout: with the inverted tree the parent
is whatever started luit (VS Code). Tests check the tree, resizing, closing
the outer terminal and the exit status, on glibc and musl builds and on
macOS.

## Builds for checking: warnings and sanitizers

CI builds luit in two ways that aren't distributed (`scripts/build.sh`):

- `--warnings` turns on configure's warnings (`-Wconversion`, `-Wshadow`
  and so on) and fails on any warning except the few in upstream's code,
  which stay as upstream has them so the fork's diff is only its own
  changes. Those are listed in `KNOWN_WARNINGS` by file, flag and the
  source line the compiler quotes after the warning (gcc and clang both
  do), so an entry holds wherever the line moves and with either compiler,
  and a warning of the same kind elsewhere in the file still counts. The
  quoted line follows its warning only if the compilers' output isn't
  interleaved, so this build keeps each one's output together (GNU make's
  `--output-sync`, or serially where make lacks it).
- `--sanitize` runs the tests under AddressSanitizer, UBSan and
  LeakSanitizer. It builds with configure's `--disable-leaks` (also on its
  own as `--leak-check`), under which luit frees its permanent memory at
  exit, so that only real leaks are reported. The fork's own permanent
  allocations are freed there too (`luit_leaks()`: the argument copies of
  `claimTitleArea()` and `expandArgsFromEnv()`, the converted task command
  line), and `luit_leaks()` skips the input and output states if creating
  them failed, rather than crashing on the way out. `--sanitize` also
  configures with `--enable-warnings`, for the attributes it defines
  (`noreturn` on `ExitProgram()` and so on): LeakSanitizer takes any pointer
  it finds in memory as a reference, and without them a stale pointer on
  `main()`'s stack hid a leak. That makes such misses less likely, not
  impossible. Reports go to files, as luit's stderr is the terminal, and the
  converter luit detaches writes its report when it exits, possibly after
  the tests return, so `scripts/check-sanitizer-reports.sh` waits for it.
  Warnings aren't checked in this build (gcc warns more falsely with
  sanitizers); the native job checks the `--leak-check` code instead.

## Known upstream bugs and fixes

- Not fixed yet: loading a table in `luitconv.c` doesn't check its
  allocations (`newLuitConv()`'s arrays, the encoding name, each row's
  text), so running out of memory then crashes luit, also mid-session when
  a program designates a charset not loaded yet. A fix has to cover all of
  them at once and leave no half-made table behind.
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
  path. Fixed it to match G2's symmetric behavior. Its `OTHER` block had
  the same kind of bug: it moved on when the charset's reverse function
  found no code (0) and wrote nothing, so the character vanished. It now
  rejects the input.
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
- `stack_gb18030()` also took a 2-byte character's trail byte as 0x40-0xFE
  except 0x80 and 0xFF, so the 144 characters with trail 0x80 (e.g. 亐,
  `0x81 0x80`) silently disappeared while 0x7F was let through, and it
  compared the second byte of a 4-byte sequence with decimal 30 instead of
  0x30-0x39.
- Every multi-byte decoder dropped bytes that couldn't make a character: a
  lead byte followed by ASCII (CP932 `0x81 0x21` showed `!`), both bytes when
  the second was `0xFF`, and `stack_gb18030()` dropped `0xFF` and broken
  4-byte sequences. EUC-JP and Big5 were ISO 2022 setups, where a broken
  sequence lost its lead byte, an invalid byte after SS2 was shown as
  Latin-1, and the C1 bytes `0x8E`/`0x8F`/`0x9B` were taken for SS2/SS3/CSI,
  though in Big5 they're lead bytes (`0x8E 0x40` showed `@`, and `0x9B`
  could swallow what followed as a control sequence). They're OTHER charsets
  now (`EUC-JP-2007`, `BIG5X` in `other_fork.c`), with the rule above.
- `gb18030_linear_to_codepoint()`: the supplementary-plane check
  (`linear >= 189000`) had no upper bound, so an invalid 4-byte sequence that
  was byte-range-valid but had a linear index past the maximum could produce
  an invalid code point beyond U+10FFFF. Added an upper bound.
- `copyIn()` also wrote a G2 (SS2) or G3 (SS3) character according to GR's
  charset type rather than G2's or G3's. EUC-JP's G2 is 1-byte JIS X 0201
  katakana next to GR's 2-byte JIS X 0208, so typed half-width katakana
  were never written (and got rejected); G3 only worked because it has
  GR's size.
- `copyIn()` wrote converted input to the pty with one non-blocking
  `write()` and ignored a short write, so whatever didn't fit was dropped:
  pasting 200 KB into a program that started reading a second later
  delivered 20 KB. Converted input is now held back and written as the pty
  takes it, and no more input is read until it's gone, while the program's
  output is still read, so neither side can block the other
  (`flushInput()`, `IO_PtyWritable`).
- `parent()` in `luit.c` relied on `restoreTermios()` (`TCSAFLUSH`) to wait
  until the outer terminal had read the last output before luit exited. On
  macOS (the classic layout), the shell's `SIGCHLD` could interrupt that
  wait (`EINTR`); luit then exited and the unread output was discarded, so a
  command that printed and exited at once showed nothing if VS Code read it
  late, e.g. while still starting up (a task's output was intermittently
  missing on macOS CI). With the reader a second late, most runs lost it.
  luit now waits with `tcdrain()`, retried on `EINTR`, before restoring the
  terminal.
- On macOS, luit waited on its ptys with `poll()`, which doesn't support
  devices there (BUGS in its man page): input went through at a few KB per
  second. It uses `select()` on macOS.
- `fromUtf8()` in `iso2022.c` masked a 4-byte UTF-8 lead with `0x03`
  instead of `0x07`, so U+100000-U+10FFFF (lead `0xF4`) were read as
  U+0000-U+FFFF.
