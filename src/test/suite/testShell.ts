import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";

const TEST_PROFILE = "terminal-any-encoding-test-shell";
const PLATFORM = process.platform === "darwin" ? "osx" : "linux";

/**
 * Points the extension at a given shell the way a user would: a profile in
 * terminal.integrated.profiles.<platform>, named by
 * terminalAnyEncoding.shellProfile.<platform>. Returns a function that undoes it.
 */
export async function useTestShell(
  shellPath: string,
): Promise<() => Promise<void>> {
  const terminalConfig = () =>
    vscode.workspace.getConfiguration("terminal.integrated");
  const extensionConfig = () =>
    vscode.workspace.getConfiguration("terminalAnyEncoding");
  await terminalConfig().update(
    `profiles.${PLATFORM}`,
    { [TEST_PROFILE]: { path: shellPath } },
    vscode.ConfigurationTarget.Global,
  );
  await extensionConfig().update(
    `shellProfile.${PLATFORM}`,
    TEST_PROFILE,
    vscode.ConfigurationTarget.Global,
  );
  return async () => {
    await extensionConfig().update(
      `shellProfile.${PLATFORM}`,
      undefined,
      vscode.ConfigurationTarget.Global,
    );
    await terminalConfig().update(
      `profiles.${PLATFORM}`,
      undefined,
      vscode.ConfigurationTarget.Global,
    );
  };
}

/** Looks a shell up on PATH, so the tests don't depend on one machine's layout. */
export function findOnPath(name: string): string | undefined {
  for (const dir of (process.env.PATH ?? "").split(path.delimiter)) {
    if (!dir) continue;
    const candidate = path.join(dir, name);
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      return candidate;
    } catch {
      // not here
    }
  }
  return undefined;
}
