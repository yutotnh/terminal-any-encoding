import * as assert from "node:assert";
import * as vscode from "vscode";
import { ENCODINGS, EncodingDefinition } from "../../encodings";
import type { TestExports } from "../../extension";

/**
 * Input the encoding can't represent never reaches the shell, and the
 * transcoder reports it to the extension (which tells the user), since the
 * bell alone is silent with VS Code's default settings.
 */
suite("rejected input is reported", function () {
  this.timeout(20000);

  test("typing a character EUC-JP can't represent reports a rejection for that encoding", async () => {
    const ext = vscode.extensions.getExtension<TestExports>(
      "yutotnh.terminal-any-encoding",
    );
    const exports = await ext!.activate();
    const encoding = ENCODINGS.find((e) => e.id === "eucjp")!;
    const built = exports.buildTerminalOptions(ext!.extensionPath, encoding);
    assert.ok(built.ok);

    const rejected = new Promise<EncodingDefinition>((resolve) => {
      const sub = exports.onDidRejectInput((e) => {
        sub.dispose();
        resolve(e);
      });
    });
    const terminal = vscode.window.createTerminal(built.options);
    try {
      await new Promise((r) => setTimeout(r, 1500));
      terminal.sendText("echo ☃", false);
      const result = await Promise.race([
        rejected,
        new Promise<undefined>((r) => setTimeout(() => r(undefined), 5000)),
      ]);
      assert.strictEqual(result?.id, "eucjp", "no rejection was reported");
    } finally {
      terminal.dispose();
    }
  });
});
