// TEMPORARY: the first task in a fresh window, with timings.
import * as os from "node:os";
import * as vscode from "vscode";
import type { TestExports } from "../../extension";
import { useTestShell } from "./testShell";

suite("PROBE first task", function () {
  this.timeout(60000);
  test("PROBE first task output", async () => {
    const t0 = Date.now();
    const ev: string[] = [];
    const ext = vscode.extensions.getExtension<TestExports>(
      "yutotnh.terminal-any-encoding",
    );
    const exports = await ext!.activate();
    const restoreShell = await useTestShell("/bin/sh");
    let output = "";
    const dataSub = vscode.window.onDidWriteTerminalData((e) => {
      ev.push(
        `data@${Date.now() - t0}:${e.terminal.name}:${JSON.stringify(e.data.slice(0, 40))}`,
      );
      output += e.data;
    });
    const openSub = vscode.window.onDidOpenTerminal((t) =>
      ev.push(`open@${Date.now() - t0}:${t.name}`),
    );
    const definition = {
      type: "terminalAnyEncoding",
      encoding: "eucjp",
      command: "printf 'out:\\306\\374\\n'; exit 3",
      options: { cwd: os.tmpdir() },
    };
    const built = exports.buildTaskExecution(ext!.extensionPath, definition);
    if (!built.ok) throw new Error("build");
    const task = new vscode.Task(
      definition,
      vscode.TaskScope.Global,
      "first",
      "terminalAnyEncoding",
      built.execution,
    );
    const ended = new Promise<number | undefined>((resolve) => {
      const sub = vscode.tasks.onDidEndTaskProcess((e) => {
        if (e.execution.task.name !== task.name) return;
        sub.dispose();
        ev.push(`end@${Date.now() - t0}:${e.exitCode}`);
        resolve(e.exitCode);
      });
    });
    ev.push(`execute@${Date.now() - t0}`);
    await vscode.tasks.executeTask(task);
    await ended;
    const deadline = Date.now() + 5000;
    while (!output.includes("out:") && Date.now() < deadline)
      await new Promise((r) => setTimeout(r, 100));
    ev.push(`done@${Date.now() - t0}`);
    console.log(
      `PROBE ${output.includes("out:") ? "OK" : "LOST"} ${ev.join(" ")}`,
    );
    dataSub.dispose();
    openSub.dispose();
    for (const t of vscode.window.terminals) t.dispose();
    await restoreShell();
  });
});
