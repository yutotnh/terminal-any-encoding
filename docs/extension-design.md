# Extension design notes

Non-obvious design decisions and constraints behind **why the code looks the
way it does** in the VS Code extension (`src/`). The transcoder side is in
[transcoder-design.md](transcoder-design.md). Implementation change history
and verification logs aren't kept here.

## Supported encodings and the UI

- Guiding rule: look and behave like a regular VS Code terminal, except
  where the transcoder makes that impossible.
- Only expose encodings whose conversion has actually been verified
  (unimplemented "variants" don't appear in the UI).
- UTF-8 isn't offered. Without conversion it would only open the same
  terminal as `+`, so going through this extension adds nothing.
- In a multi-root workspace the commands ask for the folder, like VS Code's
  "New Terminal" (skipped when `terminal.integrated.cwd` is absolute).
- Distribution is simplified around the assumption of "one binary per
  platform": the VSIX bundles exactly one platform-specific binary at the
  fixed path `transcoder/bin/luit`, so no runtime platform detection is
  needed.

## Dropdown: two profiles instead of one per encoding

The dropdown has two profiles, `🌐 Select Encoding...` (QuickPick inside
`provideTerminalProfile`) and `🌐 Default Encoding`
(`terminalAnyEncoding.defaultEncoding`, or the QuickPick when empty),
instead of one per encoding. 45 entries crowded the menu, and hiding some of
them meant writing `null` overrides into the user's global
`terminal.integrated.profiles.*` (synced, left behind on uninstall).

There's no cleaner way to ship per-encoding entries hidden by default:
`contributes.terminal.profiles` takes only `id`, `title`, `icon` and
`titleTemplate` (no `when`, microsoft/vscode#183354), and VS Code hides a
contributed profile only when that setting maps its title to `null`. Putting
the `null`s in `contributes.configurationDefaults` instead works from 1.91,
but up to 1.90 an extension's object default replaces the built-in one, so
bash, zsh etc. disappear from the default value (checked with
`inspect().defaultValue` on 1.73 through 1.140). Worth revisiting if the
lower bound reaches 1.91; a user would then re-enable an entry with
`{"extensionIdentifier", "id", "title"}` under its title.

Titles aren't localized because they're what
`terminal.integrated.defaultProfile.*` refers to; the 🌐 prefix is a
pseudo-icon, since that menu renders plain text only.

## Dismissing the profile QuickPick without an error

When the `🌐 Select Encoding...` QuickPick is dismissed there's nothing to
return, and returning `undefined` makes the extension host throw "No
terminal profile options provided", which the workbench shows as an error
notification (`createContributedTerminalProfile`'s `catch` →
`notificationService`). The extension host skips that check when the token
it passed in is cancelled (1.73 through 1.140), so the provider cancels it.
The public `CancellationToken` is read-only; this relies on it being VS
Code's internal mutable token with `cancel()`, and does nothing otherwise
(the notification comes back, nothing else breaks). An integration test
pins that the token has `cancel()`.

When this happens through `defaultProfile` with no terminal open, VS Code
still logs an internal `TypeError` (console only), since its own code
assumed a terminal was created. CI's version matrix shows it on 1.73–1.94
but not on 1.140.

## Inner shell: taken from a terminal profile

The inner shell comes from a VS Code terminal profile: the one named by
`terminalAnyEncoding.shellProfile.<platform>` (per platform, like
`terminal.integrated.defaultProfile.*`), else the default profile
(`profiles.*`: path, args, env, with `${env:...}`/`${userHome}`
resolved), so it starts the same way as in a regular terminal, e.g. as a
login shell on macOS. When that profile is unusable (another
extension's, including this one's, or a missing path) it falls back like
VS Code: `$SHELL`, with `--login` for bash/zsh on macOS
(`src/shellProfile.ts`). The setting takes a profile name rather than a
shell path, since a path can't carry args/env and would duplicate what
profiles already describe.

## Tab titles

VS Code names a tab after its pty's foreground process, read
from that process's argv[0] (`/proc/<pid>/cmdline` on Linux, cut at the
first space; re-read every 200 ms), which here is always luit. On Linux
the extension passes `-title-suffix " (EUC-JP)"` (no-break spaces, which
survive the cut), and luit rewrites its own argv[0] area (the original
argument strings, copied elsewhere at startup) to the inner pty's
foreground program plus the suffix, checking every 200 ms like VS Code.
The tab then reads like a regular one plus the encoding, `vim (EUC-JP)`,
and user title templates (`${process}`) apply. So that `ps` doesn't show
a fake `vim` (killing it would take the terminal down), the argument area
continues with `[terminal-any-encoding]` after the title, and the command
name is set back to `luit` (`prctl`), since the shell-named link would
otherwise make it `bash` for `pgrep`/`killall`. Probed in VS Code: the
title comes from argv[0], not the executable name. macOS's node-pty reads
the name the kernel recorded at exec, which can't be changed, so there the
terminal gets a fixed `name`, `<shell> (<encoding>)`.

## Running luit from a per-content copy

The VSIX's binary path contains the extension version. VS Code restores
terminals after a restart by launching the saved executable path with the
saved environment (`reviveTerminalProcesses`, `processLaunchConfig.env`),
before any extension activates, so after an update that path is gone. The
extension therefore runs a copy of luit from its `globalStorage`, one per
content: `<globalStorage>/<sha256 prefix>/luit`
(`installTranscoderCopy()`), never overwritten. Each terminal keeps
running the very luit it was created with, so the options saved with it
are always ones that luit knows: after an update, after going back to an
older version, and with windows still running an older extension host
(which, with a single shared copy, overwrote it and broke new terminals
in updated windows too). There's no option-compatibility rule to keep.
luit marks its copy's directory as used each time the extension starts
it (restored terminals included), and the extension removes copies of
other versions unused for 90 days (`pruneTranscoderCopies()`). Window
reloads don't relaunch anything; the pty host keeps the processes.

## How the locale is decided

`resolveLocaleEnv()` in `src/locale.ts` picks the locale dynamically by
cross-referencing the list of locales actually generated on the host
(`locale -a`, via `src/localeProbe.ts`). It does not hardcode a fixed
locale string per `luitEncoding`.

Why: glibc's `SUPPORTED` file notation (e.g. `de_DE/ISO-8859-1`) is not a
locale name itself but input to `localedef`, which differs from the compiled
result (`de_DE.iso88591`). Also, locales a distro generated on its own (e.g.
`ja_JP.sjis`) can't in principle be captured by a static precomputed table.

Concretely: it builds the cross product of candidate regions (region parsed
from the locale currently in effect for `LC_CTYPE` → region filled in from
`vscode.env.language` → confirmed-to-exist representative region) and
candidate charmap names (default is `luitEncoding` itself; only
`CP932`/`MACROMAN`/`euc-jp-2007` are overridden to the actual glibc charmap
name), normalizes them (lowercase + strip non-alphanumerics), and matches
against the `locale -a` results, using the first match found. If there's no
match, nothing is forced, but the extension shows a warning, since
programs then keep printing in the old locale's encoding (garbled output).
If `locale` itself can't run, nothing is forced and nothing is shown.
`locale -a` isn't cached, so a locale generated after the warning is picked
up by the next terminal.

Only `LANG` is set, the same variable VS Code's own `detectLocale` sets.
The extension is responsible for picking a sensible default, not for
overriding locale settings the user made: an `LC_*` variable (or `LC_ALL`)
outranks `LANG`, and when one in the inherited environment names a different
encoding, it's reported as a conflict and the user is warned instead of the
variable being rewritten. (Forcing every category was tried and rejected as
overreach: it fixed `date` printing UTF-8 under a UTF-8 `LC_TIME`, confirmed
by measurement, but it also froze the user's locale against their own rc
files and clobbered deliberate per-category settings.) Only variables that
name a codeset are judged; `C`/`POSIX` ones only produce ASCII, which is
fine except for `LC_ALL`/`LC_CTYPE` (`C.UTF-8` there makes the encoding
UTF-8). Shell rc files that `export LANG=...` can't be seen in advance; the
README suggests `export LANG="${LANG:-...}"`.

If no region candidate matches, any installed locale in the encoding is used
as a last resort, so the "not installed" warning is never wrong. It's only
shown when there's a locale to suggest: glibc has none for CP932 (unless a
distribution adds one) or the DOS code pages, and a warning the user can't
act on would just repeat itself.

The inherited environment used for this decision is the extension host's
plus `terminal.integrated.env.<platform>` and the default profile's `env`.

## Where the locale is applied: inside the transcoder

The variables are applied by `/usr/bin/env` between luit's `--` and the
shell (`luit ... -- /usr/bin/env LANG=... <shell> <args>`),
not through `TerminalOptions.env`. Background:

- VS Code core's `terminal.integrated.detectLocale` (default `"auto"`)
  rewrites `LANG` of the process it launches whenever it doesn't look like
  UTF-8 or EUC.
- `TerminalOptions.strictEnv: true` avoids that locally, but on
  Remote-SSH/Remote-WSL/Dev Containers it was observed not to
  ([microsoft/vscode#125389](https://github.com/microsoft/vscode/issues/125389)).
  Slipping past the check (lowercase `ja_JP.euc-jp`) doesn't work on real
  remote setups either.
- `strictEnv` also skips everything else VS Code does to the environment
  (confirmed in `server-main.js`): `terminal.integrated.env.*`, other
  extensions' EnvironmentVariableCollections (Git askpass, Python venv
  activation, ...), `TERM_PROGRAM`/`COLORTERM`, and removing internal
  `VSCODE_*` variables. The extension would have to pass a complete
  environment itself, so the extension host's internal variables would leak
  into the shell.

luit itself doesn't depend on its own locale when `-encoding` is given
(confirmed with luit's own `LANG` set to UTF-8, a legacy locale, and unset),
so whatever VS Code does to luit's environment doesn't matter, and no
`strictEnv` or remote-specific handling is needed. `TerminalOptions.env` only
carries the profile's env and the transcoder's options, merged by VS Code onto
the environment it builds as usual.

## Shell integration: left to VS Code through a shell-named link

VS Code core (`ptyHostMain.js`) decides whether and how to inject shell
integration from the basename of the executable it launches (bash, zsh,
fish, pwsh) and its arguments, and then replaces the arguments with its
injection ones and adds environment variables (`VSCODE_INJECTION`, its own
`VSCODE_NONCE`, `VSCODE_STABLE`, `VSCODE_SHELL_LOGIN`, `ZDOTDIR` for zsh,
`VSCODE_PATH_PREFIX` with other extensions' PATH contributions, environment
reporting, ...). Launched as plain `luit`, none of that happened.

So the extension launches the transcoder through a link named like the inner
shell, `<copy's directory>/shims/<shell basename>` → that copy of luit
(`shellShim()` in `src/transcoder.ts`), and passes the profile's arguments as
the terminal's arguments. VS Code then treats it exactly like a regular
terminal of that shell: it injects (or not, for arguments it doesn't support,
or when `terminal.integrated.shellIntegration.enabled` is off), with the real
nonce, so command lines are trusted. Because the arguments get replaced,
luit's own options, the shell and its leading arguments (the `env LANG=...`
wrapper) travel in `TERMINAL_ANY_ENCODING_ARGS`, one per line; luit
(`expandArgsFromEnv()`) puts them before whatever arguments it was started
with, which therefore go to the shell, and removes the variable so the shell
doesn't inherit it. Verified with a probe named `bash`: VS Code passed
`--init-file .../shellIntegration-bash.sh` and its nonce.

Copying VS Code's injection into the extension instead (scripts located under
the private `vscode.env.appRoot` layout, a per-user zsh `ZDOTDIR`, per-shell
argument maps) could only approximate it: the nonce and `VSCODE_PATH_PREFIX`
aren't available to extensions. A side effect is that injection follows
each VS Code version exactly; for example 1.73 doesn't inject into fish at all
(its pty host has no fish case), just like for its own fish terminals.

## Tasks: a task type, not the default or automation profile

Tasks (and integrated-terminal debug sessions) run in the shell of
`terminal.integrated.automationProfile.<platform>`, else of the default
profile, and VS Code skips extension-contributed profiles there, so they
never went through this extension. Two ways to change that were rejected:

- Writing `automationProfile`: debug sessions only take its `path` and
  `args` (not `env`, so the transcoder's options would need another
  channel), and a value written from a remote window lands in the local
  user settings, applying to every host. Where the path doesn't exist,
  every task and debug session fails to start, which is worse than running
  unconverted.
- Switching the default profile to a path-based profile of a generated
  script: a missing path is dropped quietly, but there's only one default,
  so every task, debug session and other extensions' terminals would get
  one encoding, and `vscode.env.shell` would point at a script that can't
  run without a terminal (luit exits with "Couldn't set terminal to raw").

So tasks get their own type, `terminalAnyEncoding`, with the encoding per
task. `resolveTask()` turns it into a `ShellExecution` whose executable is
the shell-named link, so VS Code quotes the command line for that shell, and
the transcoder's options travel in `TERMINAL_ANY_ENCODING_ARGS` as for
terminals. Naming an executable makes VS Code drop the profile's args and
the `-c` it would add (`k||F.push("-c")` in its task system), so they're
passed as `shellArgs` (`shellCommandFlag()`: `-Command` for PowerShell, `-c`
otherwise). Debug sessions stay unconverted: there's no per-session hook.

VS Code passes the task's command line, variables substituted, as the
shell's last argument, in UTF-8, so the extension adds `-encode-last-arg`
for tasks: luit converts that argument with `copyIn()` on the input state
nothing has been typed into yet (`encodeLastArg()`), i.e. as if the line
had been typed in this terminal, before it starts the shell. Only the last
argument: earlier ones are the profile's arguments and VS Code's
shell-integration script paths, which name files and must stay as they
are. A character the encoding can't represent makes luit print why and
exit 1 without running anything, like rejected input (a substitute could
change the command). VS Code reads `tasks.json` as UTF-8 (and always
writes files under `.vscode` as UTF-8, whatever `files.encoding` says), so
one saved in another encoding arrives with U+FFFD for each non-ASCII byte;
the extension refuses such a task itself, with a message naming the cause,
rather than leaving it to luit's generic "can't represent U+FFFD".

## Localization (l10n) and message-text policy

`package.json`'s `contributes` (the extension's title/description, command
titles, setting descriptions) switches automatically based on display
language via VS Code core's own NLS mechanism: `%key%` plus
`package.nls.json`/`package.nls.ja.json` (resolved statically by VS Code when
it reads `package.json`; no code involvement).

On the other hand, **strings displayed from code at runtime** — such as
`vscode.window.show*Message()`/`showQuickPick()` placeholders — don't go
through that mechanism. They're localized separately with
`vscode.l10n.t(template, ...args)`, with translations hand-written in
`l10n/bundle.l10n.ja.json`, which is what `package.json`'s `"l10n": "./l10n"`
points to (an extraction tool like `@vscode/l10n-dev` wasn't introduced,
since there are only a handful of target strings).

Constraint: `vscode.l10n.t()` uses the **literal template string written at
the call site** as the bundle key, so indirect calls — where another module
assembles a finished string and passes it to `l10n.t()` — don't get
translated. So `src/encodings.ts`/`src/transcoder.ts` (modules that keep a
policy of staying vscode-independent) return structured data
(`{ id }`/`{ missingPath }`) instead of a formatted string, and the actual
`vscode.l10n.t()` calls are centralized in `formatUnknownEncodingMessage()`/
`formatMissingTranscoderMessage()` in `src/extension.ts`, the
vscode-dependent layer. `src/test/l10n.test.ts` verifies that every
`l10n.t()` template in `extension.ts` matches the keys in
`bundle.l10n.ja.json` exactly (since hand-maintaining these means the source
and the bundle can drift apart without a runtime error, which is easy to
miss).

Message text favors brevity: since `showWarningMessage`/`showErrorMessage`
are plain text and can't be links, decorative information that can't be
clicked through (like an issue number) is left out of the message body and
deferred to documentation like the README. Error messages avoid exhaustively
enumerating every possible remedy; instead they guide the user toward the
next concrete action (e.g. running a specific command).

## Testing strategy

vscode-independent logic (`encodings.ts`/`locale.ts`/`transcoder.ts`/
`localeProbe.ts`/`shellProfile.ts`/`rejectionListener.ts`) is unit-tested
with Node's built-in `node:test`.
`extension.ts` imports `vscode` at the module level, so it can only run
inside a real VS Code (Extension Host), and is integration-tested with
`@vscode/test-electron`. `activate()` returns `TestExports`, internal
functions such as `buildTerminalOptions()`, so integration tests can verify
the extension's real logic without simulating QuickPick interactions.

## Packaging gotcha (.vscodeignore)

`vsce`'s `.vscodeignore` parsing applies negation patterns (starting with
`!`) as a group "after all exclusion patterns," regardless of where they
appear in the file (because `collectFiles()` in
`node_modules/@vscode/vsce/out/package.js` processes `ignore`/`negate` as
separate arrays). Order-dependent "undo part of an earlier pattern later"
idioms like `.gitignore` uses don't work with vsce. List anything you want
excluded as its own explicit exclusion pattern. Also, without a
`vscode:prepublish` script (`npm run compile`), `vsce package` won't compile
automatically, and it'll package a stale or nonexistent `out/`.
