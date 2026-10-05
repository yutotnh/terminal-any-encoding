/**
 * A task that runTest.ts writes into the test workspace's tasks.json, so
 * VS Code itself resolves it through the extension's provider, with the
 * script it runs. vscode-independent, since runTest.ts runs outside VS Code.
 */
export const TASK_SCRIPT_NAME = "print-eucjp.sh";

// Prints 本 in EUC-JP (CB DC) and its three arguments. A script, because
// VS Code doesn't quote a backslash in an arg, so an octal escape there
// wouldn't survive the shell.
export const TASK_SCRIPT =
  'printf \'cfg:\\313\\334:%s:%s:%s\\n\' "$1" "$2" "$3"\n';

export const CONFIGURED_TASK = {
  label: "configured encoded task",
  type: "terminalAnyEncoding",
  encoding: "eucjp",
  command: "sh",
  // No cwd: the default, the workspace folder, is where the script is.
  // 日本 is in UTF-8 here, as VS Code passes it; it reaches the script in
  // EUC-JP only if the command line is converted.
  args: [TASK_SCRIPT_NAME, "two words", "$ENC_TEST", "日本"],
  options: { env: { ENC_TEST: "from-env" } },
  problemMatcher: [],
};
