import * as assert from "node:assert";
import * as os from "node:os";
import * as vscode from "vscode";
import type { TestExports } from "../../extension";
import { CONFIGURED_TASK } from "./taskFixture";
import { findOnPath, useTestShell } from "./testShell";

const EXTENSION_ID = "yutotnh.terminal-any-encoding";

/** Runs a task and resolves with its process's exit code */
async function runTask(task: vscode.Task): Promise<number | undefined> {
  const exitCode = new Promise<number | undefined>((resolve) => {
    const sub = vscode.tasks.onDidEndTaskProcess((e) => {
      if (e.execution.task.name !== task.name) return;
      sub.dispose();
      resolve(e.exitCode);
    });
  });
  await vscode.tasks.executeTask(task);
  return exitCode;
}

/**
 * A `terminalAnyEncoding` task runs its command line in the shell through
 * the transcoder, like a regular shell task otherwise: the output reaches
 * the task terminal (and problem matchers) decoded, and the task's exit
 * code is the command's.
 */
suite("terminalAnyEncoding tasks", function () {
  this.timeout(30000);

  test("output is decoded and the exit code is the command's", async () => {
    const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
    const exports = await ext!.activate();
    const restoreShell = await useTestShell("/bin/sh");
    let output = "";
    const dataSub = vscode.window.onDidWriteTerminalData((e) => {
      output += e.data;
    });
    try {
      const definition = {
        type: "terminalAnyEncoding",
        encoding: "eucjp",
        // 日 in EUC-JP is C6 FC
        command: "printf 'out:\\306\\374\\n'; exit 3",
        // The test window has no folder for the default ${workspaceFolder}.
        options: { cwd: os.tmpdir() },
      };
      const built = exports.buildTaskExecution(ext!.extensionPath, definition);
      assert.ok(built.ok);
      const exitCode = await runTask(
        new vscode.Task(
          definition,
          vscode.TaskScope.Global,
          "encoded task",
          "terminalAnyEncoding",
          built.execution,
        ),
      );
      assert.strictEqual(exitCode, 3, JSON.stringify(output));
      assert.ok(output.includes("out:日"), JSON.stringify(output));
    } finally {
      dataSub.dispose();
      for (const t of vscode.window.terminals) t.dispose();
      await restoreShell();
    }
  });

  test("a task in tasks.json is resolved by VS Code through the provider, args and options included", async () => {
    const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
    await ext!.activate();
    const restoreShell = await useTestShell("/bin/sh");
    let output = "";
    const dataSub = vscode.window.onDidWriteTerminalData((e) => {
      output += e.data;
    });
    try {
      const task = (await vscode.tasks.fetchTasks()).find(
        (t) => t.name === CONFIGURED_TASK.label,
      );
      assert.ok(task, "VS Code didn't pick up the task from tasks.json");
      const exitCode = await runTask(task);
      assert.strictEqual(exitCode, 0, JSON.stringify(output));
      // Each arg reached the script as one word (quoted for the shell),
      // $ENC_TEST, which VS Code leaves unquoted, was expanded from
      // options.env by the shell, and 日本 was converted to EUC-JP on the
      // way in (unconverted UTF-8 would come back garbled).
      assert.ok(
        output.includes("cfg:本:two words:from-env:日本"),
        JSON.stringify(output),
      );
    } finally {
      dataSub.dispose();
      for (const t of vscode.window.terminals) t.dispose();
      await restoreShell();
    }
  });

  // pwsh is started with -Command rather than -c. Optional like fish and
  // zsh in the shell integration tests: CI's runners have it, a
  // contributor's machine may not.
  test("pwsh: the command line runs through -Command, args quoted, exit code kept", async function () {
    const pwshPath = findOnPath("pwsh");
    if (!pwshPath) this.skip();
    const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
    const exports = await ext!.activate();
    const restoreShell = await useTestShell(pwshPath!);
    let output = "";
    const dataSub = vscode.window.onDidWriteTerminalData((e) => {
      output += e.data;
    });
    try {
      for (const [name, definition, expectedExit, expectedOutput] of [
        [
          "pwsh command line",
          { command: "Write-Output 'pwsh:line'; exit 3" },
          3,
          "pwsh:line",
        ],
        [
          "pwsh args",
          { command: "Write-Output", args: ["pwsh:two words"] },
          0,
          "pwsh:two words",
        ],
      ] as const) {
        const full = {
          type: "terminalAnyEncoding",
          encoding: "eucjp",
          options: { cwd: os.tmpdir() },
          ...definition,
        };
        const built = exports.buildTaskExecution(ext!.extensionPath, full);
        assert.ok(built.ok);
        const exitCode = await runTask(
          new vscode.Task(
            full,
            vscode.TaskScope.Global,
            name,
            "terminalAnyEncoding",
            built.execution,
          ),
        );
        assert.strictEqual(exitCode, expectedExit, JSON.stringify(output));
        assert.ok(output.includes(expectedOutput), JSON.stringify(output));
      }
    } finally {
      dataSub.dispose();
      for (const t of vscode.window.terminals) t.dispose();
      await restoreShell();
    }
  });

  test("a definition without encoding or command is refused, not run", async () => {
    const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
    const exports = await ext!.activate();
    for (const definition of [
      { type: "terminalAnyEncoding", command: "true" },
      { type: "terminalAnyEncoding", encoding: "eucjp" },
      { type: "terminalAnyEncoding", encoding: "nope", command: "true" },
      // What a tasks.json saved in a non-UTF-8 encoding arrives as
      {
        type: "terminalAnyEncoding",
        encoding: "eucjp",
        command: "echo \ufffd",
      },
    ]) {
      const built = exports.buildTaskExecution(ext!.extensionPath, definition);
      assert.strictEqual(built.ok, false, JSON.stringify(definition));
    }
  });
});
