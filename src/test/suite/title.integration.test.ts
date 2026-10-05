import * as assert from "node:assert";
import * as vscode from "vscode";
import { ENCODINGS } from "../../encodings";
import type { TestExports } from "../../extension";
import { useTestShell } from "./testShell";

/**
 * On Linux the tab title follows the running program like a regular
 * terminal's, and keeps naming the encoding: "sleep (EUC-JP)".
 */
suite("tab title", function () {
  this.timeout(20000);

  test("follows the foreground program and names the encoding (Linux)", async function () {
    if (process.platform !== "linux") this.skip();
    const ext = vscode.extensions.getExtension<TestExports>(
      "yutotnh.terminal-any-encoding",
    );
    const exports = await ext!.activate();
    const encoding = ENCODINGS.find((e) => e.id === "eucjp")!;
    const restoreShell = await useTestShell("/bin/sh");
    let terminal: vscode.Terminal | undefined;
    const waitForName = async (expected: string): Promise<void> => {
      const deadline = Date.now() + 5000;
      while (terminal!.name !== expected && Date.now() < deadline) {
        await new Promise((r) => setTimeout(r, 100));
      }
      assert.strictEqual(terminal!.name, expected);
    };
    try {
      const built = exports.buildTerminalOptions(ext!.extensionPath, encoding);
      assert.ok(built.ok);
      terminal = vscode.window.createTerminal(built.options);
      await waitForName("sh (EUC-JP)");
      terminal.sendText("sleep 3", true);
      await waitForName("sleep (EUC-JP)");
      await waitForName("sh (EUC-JP)");
    } finally {
      terminal?.dispose();
      await restoreShell();
    }
  });
});
