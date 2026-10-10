/**
 * Decides which shell (path + args + profile env) to run inside the
 * transcoder, following a VS Code terminal profile so an encoded terminal
 * starts the same shell, the same way, as a regular one (e.g. macOS's login
 * shells, `-l`, which load PATH from .zprofile).
 *
 * The profile is `terminalAnyEncoding.shellProfile.<platform>` when set,
 * else the user's default profile. Either way it names an entry of
 * `terminal.integrated.profiles.<platform>`, like VS Code's own
 * `terminal.integrated.defaultProfile.<platform>`.
 *
 * vscode-independent: the caller passes in the settings it read.
 */
import * as path from "path";

export type ProfilePlatform = "linux" | "osx";

export interface ShellProfileInput {
  /** `terminalAnyEncoding.shellProfile.<platform>` (empty = use the default profile) */
  readonly shellProfileName: string | null | undefined;
  /** `terminal.integrated.defaultProfile.<platform>` */
  readonly defaultProfileName: string | null | undefined;
  /** `terminal.integrated.profiles.<platform>` (VS Code's defaults merged with the user's) */
  readonly profiles: Readonly<Record<string, unknown>>;
  readonly platform: ProfilePlatform;
  /** Used for `$SHELL` and `${env:...}` in profile paths/args */
  readonly env: Readonly<NodeJS.ProcessEnv>;
  readonly homeDir: string;
  /** Whether an executable path (absolute, or a command name to look up on PATH) exists */
  readonly exists: (executable: string) => boolean;
}

export interface ResolvedShell {
  readonly path: string;
  readonly args: readonly string[];
  /** The profile's `env` (null = unset the variable), as VS Code would apply it */
  readonly env: Readonly<Record<string, string | null>>;
}

interface ProfileObject {
  readonly path?: unknown;
  readonly args?: unknown;
  readonly env?: unknown;
}

/** Resolves the `${env:NAME}` and `${userHome}` variables VS Code allows in profiles */
function resolveVariables(
  value: string,
  env: Readonly<NodeJS.ProcessEnv>,
  homeDir: string,
): string {
  return value
    .replace(/\$\{env:([^}]+)\}/g, (_, name: string) => env[name] ?? "")
    .replace(/\$\{userHome\}/g, homeDir);
}

/**
 * VS Code's fallback when no usable profile is configured
 * (`_getUnresolvedFallbackDefaultProfile`): on Linux, the configured
 * profile whose executable has `$SHELL`'s name (so its args and env
 * apply), else `$SHELL` itself; on macOS, `$SHELL` as a login shell for
 * bash/zsh.
 */
function fallbackShell(input: ShellProfileInput): ResolvedShell {
  const executable = input.env.SHELL || "/bin/sh";
  if (input.platform === "linux") {
    const name = path.basename(executable);
    for (const profile of Object.values(input.profiles)) {
      if (!profile || typeof profile !== "object") continue;
      const rawPath = (profile as ProfileObject).path;
      const first =
        typeof rawPath === "string"
          ? rawPath
          : isStringArray(rawPath)
            ? rawPath[0]
            : undefined;
      if (first && path.basename(first) === name) {
        return {
          ...fromProfile(profile as ProfileObject, input),
          path: executable,
        };
      }
    }
    return { path: executable, args: [], env: {} };
  }
  const login = /(zsh|bash)/.test(path.basename(executable));
  return { path: executable, args: login ? ["--login"] : [], env: {} };
}

export function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every((v) => typeof v === "string");
}

export function resolveShell(input: ShellProfileInput): ResolvedShell {
  const name = input.shellProfileName || input.defaultProfileName;
  const profile = name ? input.profiles[name] : undefined;
  if (!profile || typeof profile !== "object") return fallbackShell(input);

  // Anything without a path (another extension's profile, including this
  // extension's own, which would recurse) falls back like VS Code does for
  // a profile it can't use.
  const { path: rawPath } = profile as ProfileObject;
  const candidates =
    typeof rawPath === "string"
      ? [rawPath]
      : isStringArray(rawPath)
        ? rawPath
        : undefined;
  if (!candidates) return fallbackShell(input);
  const executable = candidates
    .map((p) => resolveVariables(p, input.env, input.homeDir))
    .find((p) => input.exists(p));
  if (!executable) return fallbackShell(input);

  return { ...fromProfile(profile as ProfileObject, input), path: executable };
}

/** The profile's args and env, with variables resolved */
function fromProfile(
  profile: ProfileObject,
  input: ShellProfileInput,
): Omit<ResolvedShell, "path"> {
  const { args: rawArgs, env: rawEnv } = profile;
  const args = (isStringArray(rawArgs) ? rawArgs : []).map((a) =>
    resolveVariables(a, input.env, input.homeDir),
  );
  const env: Record<string, string | null> = {};
  if (rawEnv && typeof rawEnv === "object") {
    for (const [key, value] of Object.entries(rawEnv)) {
      if (typeof value === "string") {
        env[key] = resolveVariables(value, input.env, input.homeDir);
      } else if (value === null) {
        env[key] = null;
      }
    }
  }
  return { args, env };
}

/**
 * The flag that makes a shell run the command line given after it, as
 * VS Code's own shell tasks pass it: `-Command` for PowerShell, `-c` for
 * everything else. VS Code only adds it when the task doesn't name its own
 * shell executable, so tasks that run through the transcoder pass it
 * themselves.
 */
export function shellCommandFlag(shellPath: string): string {
  return /^(pwsh|powershell)/.test(path.basename(shellPath))
    ? "-Command"
    : "-c";
}
