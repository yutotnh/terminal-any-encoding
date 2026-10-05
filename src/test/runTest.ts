/**
 * Entry point for @vscode/test-electron. Launches a real VS Code (Extension
 * Host) and runs tests against it.
 *
 * How to run: npm run test:integration
 *   -- --vscode-version <version>  specifies which VS Code version to download
 *      (defaults to the latest stable; used to verify the engines.vscode lower bound)
 */
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { runTests } from "@vscode/test-electron";
import {
  CONFIGURED_TASK,
  TASK_SCRIPT,
  TASK_SCRIPT_NAME,
} from "./suite/taskFixture";

function readVscodeVersionArg(argv: readonly string[]): string | undefined {
  const flagIndex = argv.indexOf("--vscode-version");
  if (flagIndex === -1) return undefined;
  return argv[flagIndex + 1];
}

async function main(): Promise<void> {
  try {
    const extensionDevelopmentPath = path.resolve(__dirname, "../../");
    const version = readVscodeVersionArg(process.argv);
    // A folder with a tasks.json, so VS Code itself resolves a
    // terminalAnyEncoding task through the provider (a window without a
    // folder doesn't offer configured tasks to fetchTasks()).
    const workspaceDir = fs.mkdtempSync(
      path.join(os.tmpdir(), "terminal-any-encoding-test-workspace-"),
    );
    fs.mkdirSync(path.join(workspaceDir, ".vscode"));
    fs.writeFileSync(
      path.join(workspaceDir, ".vscode", "tasks.json"),
      JSON.stringify({ version: "2.0.0", tasks: [CONFIGURED_TASK] }),
    );
    fs.writeFileSync(path.join(workspaceDir, TASK_SCRIPT_NAME), TASK_SCRIPT);

    const run = (tests: string) =>
      runTests({
        extensionDevelopmentPath,
        extensionTestsPath: path.resolve(__dirname, tests),
        version,
        launchArgs: [
          workspaceDir,
          // Tasks don't run in Restricted Mode.
          "--disable-workspace-trust",
          "--disable-extensions",
          // A fresh user data dir per run: the tests change user settings,
          // and a shared one would carry state over from earlier runs (or
          // other VS Code versions) and hide or fake failures.
          `--user-data-dir=${fs.mkdtempSync(
            path.join(os.tmpdir(), "terminal-any-encoding-test-user-data-"),
          )}`,
          // Enables onDidWriteTerminalData (a proposed API) only for test
          // runs, so tests can read what a terminal renders. Not used by
          // product code.
          "--enable-proposed-api=yutotnh.terminal-any-encoding",
        ],
      });
    // Alone first: how the extension activates can only be checked before
    // anything else has activated it.
    await run("./onTaskType/index");
    await run("./suite/index");
  } catch (err) {
    console.error("Extension integration tests failed:", err);
    process.exit(1);
  }
}

void main();
