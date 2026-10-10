/**
 * Extension entry point.
 *
 * Provides:
 *   - The `terminalAnyEncoding.openTerminal` command
 *     (opens a terminal after picking an encoding via QuickPick. If an
 *     encoding id is passed as a command argument, it's used instead and the
 *     QuickPick is skipped)
 *   - The `terminalAnyEncoding.openDefaultEncodingTerminal` command
 *     (opens a terminal with the `defaultEncoding` setting, or shows the
 *     QuickPick when it's empty)
 *   - Two terminal profiles for the dropdown's "+": "Select Encoding..."
 *     (QuickPick) and "Default Encoding" (same as the command above, and the
 *     one to point `terminal.integrated.defaultProfile.*` at)
 *   - The `terminalAnyEncoding` task type for tasks.json
 *
 * This file holds what depends on the vscode API: reading settings,
 * messages and their localization, and building the terminals and tasks.
 * The decisions themselves (which encodings exist, the locale, the shell,
 * where the transcoder is) are in encodings.ts, locale.ts, shellProfile.ts
 * and transcoder.ts, which don't depend on vscode and are unit-tested.
 */
import * as crypto from "crypto";
import * as os from "os";
import * as path from "path";
import * as vscode from "vscode";
import {
  ENCODINGS,
  EncodingDefinition,
  UnknownEncodingReason,
  encodingShortName,
  groupForPicker,
  recordRecentEncoding,
  validateEncodingId,
} from "./encodings";
import {
  LocaleResolution,
  resolveLocaleEnv,
  suggestLocaleName,
} from "./locale";
import { listAvailableLocales } from "./localeProbe";
import {
  installTranscoderCopy,
  isExecutableFile,
  pruneTranscoderCopies,
  resolveTranscoder,
  shellShim,
} from "./transcoder";
import {
  RejectionListener,
  RejectionReport,
  prepareNotifyDir,
} from "./rejectionListener";
import {
  ProfilePlatform,
  ResolvedShell,
  isStringArray,
  resolveShell,
  shellCommandFlag,
} from "./shellProfile";

const EXTENSION_ID = "terminalAnyEncoding";
// The task type in tasks.json (see EncodingTaskProvider)
const TASK_TYPE = EXTENSION_ID;
// The environment variable the transcoder takes its options from (see
// luit's expandArgsFromEnv)
const TRANSCODER_ARGS_ENV = "TERMINAL_ANY_ENCODING_ARGS";
// Tells apart the terminals opened from profiles (see showWhenOpened); the
// transcoder removes it, so the shell doesn't inherit it
const LAUNCH_ID_ENV = "TERMINAL_ANY_ENCODING_LAUNCH_ID";
// How long showWhenOpened waits for a terminal that may never be created
// (e.g. a failed launch)
const SHOW_WHEN_OPENED_TIMEOUT_MS = 30000;

// Encodings used to open terminals, most recent first (the picker lists
// them first). Kept here and only written to globalState (read once, in
// activate()): globalState echoes each write back from VS Code's side, and
// a late echo of an earlier write can replace a newer value in memory, so
// reading it back could list what was used before the last terminal.
let recentEncodingIds: string[] = [];
let recentEncodingsStore: vscode.Memento | undefined;
const RECENT_ENCODINGS_KEY = "recentEncodings";

// Locale warnings already shown during this activation (≈ per VS Code
// session), keyed by encoding and problem, so opening several terminals
// doesn't repeat them.
const localeWarningsShown = new Set<string>();

// This version's copy of the transcoder (see installTranscoderCopy),
// set up in activate().
let transcoderCopyPath: string | undefined;
// Copies of other versions no terminal has started from for this long are
// removed (a restored terminal counts as a start).
const COPY_MAX_UNUSED_MS = 90 * 24 * 60 * 60 * 1000;

// The directory the transcoder reports rejected input to (see
// RejectionListener), set up in activate(); without it, terminals just ring
// the bell.
let rejectionNotifyDir: string | undefined;
// At most one rejected-input notification per encoding in this long
const REJECTION_NOTICE_INTERVAL_MS = 5000;
const lastRejectionNotice = new Map<string, number>();
const rejectionEmitter = new vscode.EventEmitter<EncodingDefinition>();

function formatUnknownEncodingMessage(reason: UnknownEncodingReason): string {
  return vscode.l10n.t(
    'Unknown encoding: "{0}". Run "Open Terminal with Encoding..." to see the supported encodings.',
    reason.id,
  );
}

function formatMissingTranscoderMessage(missingPath: string): string {
  return vscode.l10n.t(
    "Transcoder not found ({0}). Try reinstalling the extension, or report it: https://github.com/yutotnh/terminal-any-encoding/issues",
    missingPath,
  );
}

function profilePlatform(): ProfilePlatform {
  return process.platform === "darwin" ? "osx" : "linux";
}

/** Whether an executable exists: an absolute path, or a command name on PATH */
function executableExists(executable: string): boolean {
  const candidates = path.isAbsolute(executable)
    ? [executable]
    : (process.env.PATH ?? "")
        .split(path.delimiter)
        .filter(Boolean)
        .map((dir) => path.join(dir, executable));
  return candidates.some(isExecutableFile);
}

/**
 * The shell a regular VS Code terminal would start: the profile named by
 * `terminalAnyEncoding.shellProfile.<platform>`, else the default profile.
 */
function resolveInnerShell(): ResolvedShell {
  const platform = profilePlatform();
  const terminalConfig = vscode.workspace.getConfiguration(
    "terminal.integrated",
  );
  return resolveShell({
    shellProfileName: vscode.workspace
      .getConfiguration(EXTENSION_ID)
      .get<string | null>(`shellProfile.${platform}`),
    defaultProfileName: terminalConfig.get<string | null>(
      `defaultProfile.${platform}`,
    ),
    profiles: terminalConfig.get<Record<string, unknown>>(
      `profiles.${platform}`,
      {},
    ),
    platform,
    env: process.env,
    homeDir: os.homedir(),
    exists: executableExists,
  });
}

/**
 * Approximates the environment VS Code gives the terminal, for deciding the
 * locale: the extension host's environment plus
 * `terminal.integrated.env.<platform>` and the profile's env. `null` there
 * means "unset", as in VS Code.
 */
function terminalBaseEnv(
  profileEnv: Readonly<Record<string, string | null>>,
): NodeJS.ProcessEnv {
  const platformEnv = vscode.workspace
    .getConfiguration("terminal.integrated")
    .get<Record<string, string | null>>(`env.${profilePlatform()}`, {});
  const env: NodeJS.ProcessEnv = { ...process.env };
  for (const [name, value] of Object.entries({
    ...platformEnv,
    ...profileEnv,
  })) {
    if (value === null) delete env[name];
    else env[name] = value;
  }
  return env;
}

/** How to start the inner shell through the transcoder, for a terminal or a task */
interface TranscodedShell {
  /** The shell-named link to the transcoder (see shellShim), or the transcoder itself */
  readonly executable: string;
  readonly shell: ResolvedShell;
  /** The profile's env plus the transcoder's options */
  readonly env: Record<string, string | null>;
  readonly locale: LocaleResolution;
}

function buildTranscodedShell(
  extensionPath: string,
  encoding: EncodingDefinition,
  extra: {
    /** Appended to the inner program's name in the tab title (Linux) */
    readonly titleSuffix?: string;
    /** The last argument is a task's command line, converted as if typed */
    readonly encodeLastArg?: boolean;
  },
): { ok: true; launch: TranscodedShell } | { ok: false; message: string } {
  const shell = resolveInnerShell();

  const resolution = resolveTranscoder(extensionPath);
  if (!resolution.ok) {
    return {
      ok: false,
      message: formatMissingTranscoderMessage(resolution.missingPath),
    };
  }

  const locale = resolveLocaleEnv(
    encoding.luitEncoding,
    terminalBaseEnv(shell.env),
    listAvailableLocales(),
    vscode.env.language,
  );

  // The transcoder's own options, then the shell and the leading part of
  // its command line, one per line (see TRANSCODER_ARGS_ENV). LANG is
  // applied by `env` inside the transcoder, i.e. only to the shell, rather
  // than through TerminalOptions.env: VS Code core rewrites LANG of the
  // process it launches (terminal.integrated.detectLocale, which ignores
  // strictEnv on remote connections, microsoft/vscode#125389), and opting
  // out with strictEnv would also drop terminal.integrated.env.* and other
  // extensions' environment contributions. The transcoder itself doesn't
  // depend on its own locale (`-encoding` is explicit).
  const transcoderArgs = [
    "-encoding",
    encoding.luitEncoding,
    ...(rejectionNotifyDir ? ["-notify", rejectionNotifyDir] : []),
    ...(extra.titleSuffix ? ["-title-suffix", extra.titleSuffix] : []),
    ...(extra.encodeLastArg ? ["-encode-last-arg"] : []),
    "--",
    ...(locale.kind === "matched"
      ? ["/usr/bin/env", `LANG=${locale.locale}`]
      : []),
    shell.path,
  ];

  // Launched through a link named like the shell, VS Code sets up shell
  // integration itself, exactly as for a regular terminal of that profile:
  // it passes the profile's arguments through or replaces them with its
  // injection, and the transcoder hands them to the shell (see shellShim).
  // The link lives next to the copy; without one, the bundled transcoder is
  // launched directly.
  const executable = transcoderCopyPath
    ? (shellShim(transcoderCopyPath, shell.path) ?? transcoderCopyPath)
    : resolution.path;

  return {
    ok: true,
    launch: {
      executable,
      shell,
      env: {
        ...shell.env,
        [TRANSCODER_ARGS_ENV]: transcoderArgs.join("\n"),
      },
      locale,
    },
  };
}

function buildTerminalOptions(
  extensionPath: string,
  encoding: EncodingDefinition,
):
  | { ok: true; options: vscode.TerminalOptions; locale: LocaleResolution }
  | { ok: false; message: string } {
  // On Linux the tab title follows the running program like a regular
  // terminal's, with the encoding added by the transcoder. " (EUC-JP)" has
  // no-break spaces: VS Code cuts a process title at the first ordinary
  // space.
  const followsTitle = process.platform === "linux";
  const built = buildTranscodedShell(extensionPath, encoding, {
    titleSuffix: followsTitle
      ? `\u00a0(${encodingShortName(encoding).replace(/ /g, "\u00a0")})`
      : undefined,
  });
  if (!built.ok) return built;
  const { executable, shell, env, locale } = built.launch;
  return {
    ok: true,
    options: {
      // Elsewhere the process name VS Code reads can't be changed after
      // exec, so the title is fixed: the shell plus the encoding.
      ...(followsTitle
        ? {}
        : {
            name: `${path.basename(shell.path)} (${encodingShortName(encoding)})`,
          }),
      iconPath: new vscode.ThemeIcon("globe"),
      shellPath: executable,
      shellArgs: [...shell.args],
      // Merged by VS Code onto the environment it builds as usual
      // (terminal.integrated.env.*, other extensions' contributions, ...).
      env,
    },
    locale,
  };
}

/**
 * A task of type `terminalAnyEncoding` in tasks.json: a shell task whose
 * shell runs through the transcoder, like the terminals. The fields mirror
 * VS Code's own shell tasks; `taskDefinitions` in package.json is the
 * schema.
 */
interface EncodingTaskDefinition extends vscode.TaskDefinition {
  readonly encoding?: unknown;
  readonly command?: unknown;
  readonly args?: unknown;
  readonly options?: { readonly cwd?: unknown; readonly env?: unknown };
}

/**
 * The execution for an encoding task: the same shell as a terminal, started
 * through the transcoder (see buildTranscodedShell), with the command line
 * passed the way VS Code's own shell tasks pass it.
 */
function buildTaskExecution(
  extensionPath: string,
  definition: EncodingTaskDefinition,
):
  | {
      ok: true;
      execution: vscode.ShellExecution;
      encoding: EncodingDefinition;
      locale: LocaleResolution;
    }
  | { ok: false; message: string } {
  const { encoding: id, command, args, options } = definition;
  const env = options?.env;
  if (
    typeof id !== "string" ||
    typeof command !== "string" ||
    (args !== undefined && !isStringArray(args)) ||
    (options?.cwd !== undefined && typeof options.cwd !== "string") ||
    (env !== undefined &&
      (typeof env !== "object" ||
        env === null ||
        !Object.values(env).every((v) => typeof v === "string")))
  ) {
    return {
      ok: false,
      message: vscode.l10n.t(
        'A "{0}" task needs "encoding" and "command" (strings), and optionally "args" (strings) and "options" ("cwd", "env").',
        TASK_TYPE,
      ),
    };
  }
  // VS Code reads tasks.json as UTF-8, so a file saved in another encoding
  // arrives with U+FFFD in place of each non-ASCII byte: the original
  // command is lost (the transcoder would refuse it, as U+FFFD can't be
  // encoded, but without saying why).
  if ([command, ...(args ?? [])].some((v) => v.includes("\ufffd"))) {
    return {
      ok: false,
      message: vscode.l10n.t(
        'The command of a "{0}" task has characters that couldn\'t be read. Save tasks.json as UTF-8 (VS Code does when you edit it there).',
        TASK_TYPE,
      ),
    };
  }
  const validated = validateEncodingId(id);
  if (!validated.ok) {
    return {
      ok: false,
      message: formatUnknownEncodingMessage(validated.reason),
    };
  }
  // Task terminals are named by VS Code, so no title suffix. VS Code
  // passes the command line (with ${file} and the like substituted) as the
  // shell's last argument, in UTF-8; the transcoder converts it as if it
  // were typed, and refuses to run it if it can't be represented.
  const built = buildTranscodedShell(extensionPath, validated.encoding, {
    encodeLastArg: true,
  });
  if (!built.ok) return built;
  const { executable, shell, locale } = built.launch;
  // Tasks can't unset a variable, so the profile's `null`s are dropped.
  const launchEnv: Record<string, string> = {};
  for (const [name, value] of Object.entries(built.launch.env)) {
    if (value !== null) launchEnv[name] = value;
  }
  const executionOptions: vscode.ShellExecutionOptions = {
    // The shell-named link makes VS Code quote the command line for that
    // shell. Naming an executable also makes VS Code drop the profile's
    // args and the `-c` it would add, so they're passed here, as for a
    // regular shell task.
    executable,
    shellArgs: [...shell.args, shellCommandFlag(shell.path)],
    ...(typeof options?.cwd === "string" ? { cwd: options.cwd } : {}),
    env: { ...launchEnv, ...(env as Record<string, string> | undefined) },
  };
  return {
    ok: true,
    execution: args
      ? new vscode.ShellExecution(command, args, executionOptions)
      : new vscode.ShellExecution(command, executionOptions),
    encoding: validated.encoding,
    locale,
  };
}

/**
 * Resolves `terminalAnyEncoding` tasks from tasks.json. It contributes no
 * tasks of its own: which commands to run, and in which encoding, is the
 * user's to write.
 */
class EncodingTaskProvider implements vscode.TaskProvider {
  constructor(private readonly extensionPath: string) {}

  provideTasks(): vscode.Task[] {
    return [];
  }

  resolveTask(task: vscode.Task): vscode.Task | undefined {
    const built = buildTaskExecution(
      this.extensionPath,
      task.definition as EncodingTaskDefinition,
    );
    if (!built.ok) {
      void vscode.window.showErrorMessage(built.message);
      return undefined;
    }
    void maybeWarnAboutLocale(built.encoding, built.locale);
    return new vscode.Task(
      task.definition,
      task.scope ?? vscode.TaskScope.Workspace,
      task.name,
      task.source,
      built.execution,
      task.problemMatchers,
    );
  }
}

/**
 * Tells the user when the shell won't actually end up in the encoding's
 * locale: the host has no such locale (nothing is forced then), or an
 * inherited `LC_*` variable naming another encoding outranks the `LANG` set
 * here. Either way programs print in another encoding, which shows up as
 * garbled text. At most once per encoding and problem per session, and not
 * at all once `warnAboutLocale` is off ("Don't Show Again" turns it off).
 * Fire-and-forget, so it doesn't block opening the terminal.
 */
async function maybeWarnAboutLocale(
  encoding: EncodingDefinition,
  locale: LocaleResolution,
): Promise<void> {
  const config = vscode.workspace.getConfiguration(EXTENSION_ID);
  if (!config.get<boolean>("warnAboutLocale", true)) return;

  let key: string;
  let message: string;
  if (locale.kind === "noMatch") {
    // Only when there's a locale to suggest: glibc has none at all for
    // some encodings (see suggestLocaleName), and a warning the user can't
    // act on would just repeat itself (README explains).
    const suggestion = suggestLocaleName(encoding.luitEncoding);
    if (!suggestion) return;
    key = `${encoding.id}:noMatch`;
    message = vscode.l10n.t(
      "No {0} locale is installed on this host, so programs in this terminal may print garbled non-ASCII text. Generate one (e.g. {1}) and open a new terminal.",
      encoding.label,
      suggestion,
    );
  } else if (locale.kind === "matched" && locale.conflicts.length > 0) {
    key = `${encoding.id}:conflict:${locale.conflicts.join(",")}`;
    message = vscode.l10n.t(
      "{0} in your environment names a different encoding and takes precedence over LANG={1}, so programs in this terminal may print garbled text. Unset it or set it to {1}.",
      locale.conflicts.join(", "),
      locale.locale,
    );
  } else {
    return;
  }
  if (localeWarningsShown.has(key)) return;
  localeWarningsShown.add(key);

  const SUPPRESS = vscode.l10n.t("Don't Show Again");
  const selection = await vscode.window.showWarningMessage(message, SUPPRESS);
  if (selection === SUPPRESS) {
    await config.update(
      "warnAboutLocale",
      false,
      vscode.ConfigurationTarget.Global,
    );
  }
}

async function pickEncoding(): Promise<EncodingDefinition | undefined> {
  const toItem = (e: EncodingDefinition) => ({
    label: e.label,
    // The id is what settings and keybinding arguments take, so show it
    // where users can find it.
    description: e.id,
    encoding: e,
  });
  const { recent, others } = groupForPicker(recentEncodingIds);
  type Item = vscode.QuickPickItem & { encoding?: EncodingDefinition };
  const separator = (label: string): Item => ({
    label,
    kind: vscode.QuickPickItemKind.Separator,
  });
  // Like the Command Palette: "recently used", then "other ..." (the
  // separators only when there is something recent).
  const items: Item[] =
    recent.length > 0
      ? [
          separator(vscode.l10n.t("recently used")),
          ...recent.map(toItem),
          separator(vscode.l10n.t("other encodings")),
          ...others.map(toItem),
        ]
      : others.map(toItem);
  const picked = await vscode.window.showQuickPick(items, {
    placeHolder: vscode.l10n.t("Select an encoding"),
  });
  return picked?.encoding;
}

/** Validates an encoding id, showing the error for an unknown one */
function lookUpEncoding(id: string): EncodingDefinition | undefined {
  const validated = validateEncodingId(id);
  if (!validated.ok) {
    void vscode.window.showErrorMessage(
      formatUnknownEncodingMessage(validated.reason),
    );
    return undefined;
  }
  return validated.encoding;
}

/** The `defaultEncoding` setting, or the QuickPick when it's empty */
async function defaultOrPickedEncoding(): Promise<
  EncodingDefinition | undefined
> {
  const configured = vscode.workspace
    .getConfiguration(EXTENSION_ID)
    .get<string>("defaultEncoding", "");
  return configured ? lookUpEncoding(configured) : pickEncoding();
}

/**
 * Builds the terminal options for an encoding, reporting problems to the
 * user. Shared by the commands and the profile providers.
 */
function prepareTerminal(
  extensionPath: string,
  encoding: EncodingDefinition,
): vscode.TerminalOptions | undefined {
  const built = buildTerminalOptions(extensionPath, encoding);
  if (!built.ok) {
    // If the transcoder is missing, show guidance instead of mojibake.
    void vscode.window.showErrorMessage(built.message);
    return undefined;
  }
  void maybeWarnAboutLocale(encoding, built.locale);
  recentEncodingIds = recordRecentEncoding(recentEncodingIds, encoding.id);
  void recentEncodingsStore?.update(RECENT_ENCODINGS_KEY, recentEncodingIds);
  return built.options;
}

/**
 * Like VS Code's "New Terminal" in a multi-root workspace: asks which folder
 * to start in, unless terminal.integrated.cwd makes them all the same.
 * Returns null when the user dismisses the question; undefined means "let
 * VS Code decide" (a single folder, or none).
 */
async function chooseCwd(): Promise<vscode.Uri | undefined | null> {
  const folders = vscode.workspace.workspaceFolders ?? [];
  if (folders.length <= 1) return undefined;
  const configuredCwd = vscode.workspace
    .getConfiguration("terminal.integrated")
    .get<string>("cwd", "");
  if (configuredCwd && path.isAbsolute(configuredCwd)) return undefined;
  const folder = await vscode.window.showWorkspaceFolderPick({
    placeHolder: vscode.l10n.t(
      "Select current working directory for new terminal",
    ),
  });
  return folder ? folder.uri : null;
}

async function openTerminal(
  extensionPath: string,
  encoding: EncodingDefinition | undefined,
): Promise<void> {
  if (!encoding) return;
  const cwd = await chooseCwd();
  if (cwd === null) return;
  const options = prepareTerminal(extensionPath, encoding);
  if (!options) return;
  const terminal = vscode.window.createTerminal(
    cwd ? { ...options, cwd } : options,
  );
  terminal.show();
}

/**
 * Makes VS Code drop a profile request quietly. Returning undefined from
 * provideTerminalProfile makes the extension host throw "No terminal profile
 * options provided", which the workbench shows as an error notification;
 * but if the token it passed in is cancelled, it returns without a word
 * (checked in 1.73 through 1.140). The public CancellationToken is
 * read-only, so this relies on the object actually being VS Code's
 * internal mutable token, which has cancel(). If that ever changes, this
 * does nothing and the old behavior (the notification) comes back; no
 * terminal is opened either way.
 */
function cancelProfileRequest(token: vscode.CancellationToken): void {
  const cancel = (token as { cancel?: unknown }).cancel;
  if (typeof cancel === "function") cancel.call(token);
}

/**
 * A dropdown entry. When the QuickPick is dismissed (or an error was already
 * shown), there's no terminal to create; the request is cancelled instead
 * of failing (see cancelProfileRequest).
 */
class EncodingTerminalProfileProvider
  implements vscode.TerminalProfileProvider
{
  constructor(
    private readonly extensionPath: string,
    private readonly chooseEncoding: () => Promise<
      EncodingDefinition | undefined
    >,
  ) {}

  async provideTerminalProfile(
    token: vscode.CancellationToken,
  ): Promise<vscode.TerminalProfile | undefined> {
    const encoding = await this.chooseEncoding();
    const options = encoding
      ? prepareTerminal(this.extensionPath, encoding)
      : undefined;
    if (!options) {
      cancelProfileRequest(token);
      return undefined;
    }
    const launchId = crypto.randomUUID();
    showWhenOpened(launchId);
    return new vscode.TerminalProfile({
      ...options,
      env: { ...options.env, [LAUNCH_ID_ENV]: launchId },
    });
  }
}

/**
 * Makes the terminal VS Code is about to create from a profile the active
 * one. VS Code activates the last terminal in the list as soon as the
 * extension host has handed over the options, without waiting for the
 * terminal to be created; over a remote connection (e.g. WSL) it isn't in
 * the list yet, so the previously active terminal stays active. Showing it
 * once it has opened comes after that, since the open event is sent only
 * after the terminal exists. The terminal is recognized by launchId, which
 * its options carry in LAUNCH_ID_ENV, so another terminal of the same
 * encoding opening meanwhile isn't taken for it.
 */
function showWhenOpened(launchId: string): void {
  const subscription = vscode.window.onDidOpenTerminal((terminal) => {
    const opened = terminal.creationOptions as vscode.TerminalOptions;
    if (opened.env?.[LAUNCH_ID_ENV] !== launchId) return;
    stop();
    terminal.show();
  });
  const timer = setTimeout(stop, SHOW_WHEN_OPENED_TIMEOUT_MS);
  function stop(): void {
    clearTimeout(timer);
    subscription.dispose();
  }
}

/**
 * The transcoder rejected input that the encoding can't represent (nothing
 * reached the shell). Every window hears about every rejection, so only the
 * one owning that terminal speaks up, and at most once per
 * REJECTION_NOTICE_INTERVAL_MS per encoding, so a burst of rejected
 * keystrokes doesn't stack up notifications.
 */
async function handleRejectedInput(report: RejectionReport): Promise<void> {
  const encoding = ENCODINGS.find(
    (e) => e.luitEncoding === report.luitEncoding,
  );
  if (!encoding) return;
  const ownProcessIds = await Promise.all(
    vscode.window.terminals.map((t) => t.processId),
  );
  if (!ownProcessIds.includes(report.pid)) return;
  rejectionEmitter.fire(encoding);
  const now = Date.now();
  const sinceLast = now - (lastRejectionNotice.get(encoding.id) ?? 0);
  if (sinceLast < REJECTION_NOTICE_INTERVAL_MS) return;
  lastRejectionNotice.set(encoding.id, now);
  void vscode.window.showWarningMessage(
    report.character
      ? vscode.l10n.t(
          "The input wasn't sent because {0} can't represent \"{1}\".",
          encodingShortName(encoding),
          report.character,
        )
      : vscode.l10n.t(
          "The input wasn't sent because it contains characters that {0} can't represent.",
          encodingShortName(encoding),
        ),
  );
}

/**
 * Test-only surface (used from src/test/suite/*.integration.test.ts via
 * `getExtension(...).exports`), not an API for other extensions. It lets
 * the integration tests check what the commands, profiles and tasks build
 * without going through a QuickPick, and reset or observe the state they
 * leave behind.
 */
export interface TestExports {
  buildTerminalOptions: typeof buildTerminalOptions;
  buildTaskExecution: typeof buildTaskExecution;
  cancelProfileRequest: typeof cancelProfileRequest;
  onDidRejectInput: vscode.Event<EncodingDefinition>;
  clearRecentEncodings: () => Thenable<void>;
}

export async function activate(
  context: vscode.ExtensionContext,
): Promise<TestExports> {
  const { extensionPath } = context;
  recentEncodingsStore = context.globalState;
  recentEncodingIds = context.globalState.get<string[]>(
    RECENT_ENCODINGS_KEY,
    [],
  );
  const bundled = resolveTranscoder(extensionPath);
  if (bundled.ok) {
    transcoderCopyPath = installTranscoderCopy(
      bundled.path,
      context.globalStorageUri.fsPath,
    );
    pruneTranscoderCopies(
      context.globalStorageUri.fsPath,
      transcoderCopyPath,
      COPY_MAX_UNUSED_MS,
    );
  }
  const rejectionListener = new RejectionListener(
    (report) => void handleRejectedInput(report),
  );
  const notifyDir = prepareNotifyDir();
  if (notifyDir && (await rejectionListener.start(notifyDir))) {
    rejectionNotifyDir = notifyDir;
  }
  context.subscriptions.push(rejectionListener, rejectionEmitter);
  context.subscriptions.push(
    vscode.commands.registerCommand(
      `${EXTENSION_ID}.openTerminal`,
      async (encodingIdArg?: string) => {
        await openTerminal(
          extensionPath,
          encodingIdArg ? lookUpEncoding(encodingIdArg) : await pickEncoding(),
        );
      },
    ),
    vscode.commands.registerCommand(
      `${EXTENSION_ID}.openDefaultEncodingTerminal`,
      async () => {
        await openTerminal(extensionPath, await defaultOrPickedEncoding());
      },
    ),
    vscode.window.registerTerminalProfileProvider(
      `${EXTENSION_ID}.select`,
      new EncodingTerminalProfileProvider(extensionPath, pickEncoding),
    ),
    vscode.tasks.registerTaskProvider(
      TASK_TYPE,
      new EncodingTaskProvider(extensionPath),
    ),
    vscode.window.registerTerminalProfileProvider(
      `${EXTENSION_ID}.default`,
      new EncodingTerminalProfileProvider(
        extensionPath,
        defaultOrPickedEncoding,
      ),
    ),
  );

  return {
    buildTerminalOptions,
    buildTaskExecution,
    cancelProfileRequest,
    onDidRejectInput: rejectionEmitter.event,
    clearRecentEncodings: () => {
      recentEncodingIds = [];
      return context.globalState.update(RECENT_ENCODINGS_KEY, undefined);
    },
  };
}
