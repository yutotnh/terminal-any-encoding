import * as assert from "node:assert";
import * as fs from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";
import { ENCODINGS } from "../../encodings";
import type { TestExports } from "../../extension";
import { useTestShell } from "./testShell";

/**
 * The process VS Code started for an encoded terminal is the shell itself,
 * with the converter detached, so VS Code's child-process check (confirm on
 * close), exit code and working directory work as in a regular terminal.
 */
suite("process tree matches a regular terminal", function () {
  this.timeout(20000);

  test("the terminal's process is the shell, and the converter isn't its child", async function () {
    if (process.platform !== "linux") this.skip();
    const ext = vscode.extensions.getExtension<TestExports>(
      "yutotnh.terminal-any-encoding",
    );
    const exports = await ext!.activate();
    const encoding = ENCODINGS.find((e) => e.id === "eucjp")!;
    const restoreShell = await useTestShell("/bin/sh");
    let terminal: vscode.Terminal | undefined;
    try {
      const built = exports.buildTerminalOptions(ext!.extensionPath, encoding);
      assert.ok(built.ok);
      terminal = vscode.window.createTerminal(built.options);
      const pid = await terminal.processId;
      await new Promise((r) => setTimeout(r, 1500));
      const exe = path.basename(fs.readlinkSync(`/proc/${pid}/exe`));
      const children = fs
        .readFileSync(`/proc/${pid}/task/${pid}/children`, "utf8")
        .split(/\s+/)
        .filter(Boolean)
        .map((c) => path.basename(fs.readlinkSync(`/proc/${c}/exe`)));
      assert.notStrictEqual(exe, "luit");
      assert.ok(!children.includes("luit"), JSON.stringify(children));
    } finally {
      terminal?.dispose();
      await restoreShell();
    }
  });
});
