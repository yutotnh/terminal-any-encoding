import * as assert from "node:assert";
import * as vscode from "vscode";
import { ENCODINGS, EncodingDefinition } from "../../encodings";
import type { TestExports } from "../../extension";

const EXTENSION_ID = "yutotnh.terminal-any-encoding";

function waitFor(
  predicate: () => boolean,
  timeoutMs: number,
  intervalMs = 200,
): Promise<void> {
  return new Promise((resolve, reject) => {
    const start = Date.now();
    const tick = () => {
      if (predicate()) {
        resolve();
        return;
      }
      if (Date.now() - start > timeoutMs) {
        reject(new Error("Timed out: condition was never satisfied"));
        return;
      }
      setTimeout(tick, intervalMs);
    };
    tick();
  });
}

/**
 * Opens 3 tabs simultaneously with different encodings (eucjp / shiftjis /
 * gbk) and confirms each renders correctly, independently of the others.
 */
suite("open terminals with multiple encodings simultaneously", function () {
  this.timeout(60000);

  test("opening EUC-JP / CP932 / GBK simultaneously still renders each correctly, independently", async () => {
    const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
    assert.ok(ext);
    const exports = await ext!.activate();

    const targets: {
      encoding: EncodingDefinition;
      command: string;
      expect: string;
    }[] = [
      {
        encoding: ENCODINGS.find((e) => e.id === "eucjp")!,
        command: "echo 日本語EUC髙鷗",
        expect: "日本語EUC髙鷗",
      },
      {
        encoding: ENCODINGS.find((e) => e.id === "shiftjis")!,
        command: "echo 日本語CP932髙",
        expect: "日本語CP932髙",
      },
      {
        encoding: ENCODINGS.find((e) => e.id === "gbk")!,
        command: "echo 简体中文GBK",
        expect: "简体中文GBK",
      },
    ];

    const buffers = new Map<vscode.Terminal, string>();
    const terminals: vscode.Terminal[] = [];

    const sub = vscode.window.onDidWriteTerminalData((e) => {
      const prev = buffers.get(e.terminal) ?? "";
      buffers.set(e.terminal, prev + e.data);
    });

    try {
      // Open all 3 simultaneously
      for (const t of targets) {
        const built = exports.buildTerminalOptions(
          ext!.extensionPath,
          t.encoding,
        );
        assert.strictEqual(
          built.ok,
          true,
          `failed to build terminal options for ${t.encoding.id}`,
        );
        if (!built.ok) continue;
        const terminal = vscode.window.createTerminal(built.options);
        buffers.set(terminal, "");
        terminals.push(terminal);
      }
      assert.strictEqual(terminals.length, 3);

      // Wait for the shells to start before sending commands
      await new Promise((r) => setTimeout(r, 1500));
      for (let i = 0; i < targets.length; i++) {
        terminals[i].sendText(targets[i].command, true);
      }

      // Wait for each expected string to appear, independently
      await waitFor(
        () =>
          targets.every((t, i) =>
            (buffers.get(terminals[i]) ?? "").includes(t.expect),
          ),
        30000,
      );

      for (let i = 0; i < targets.length; i++) {
        const data = buffers.get(terminals[i]) ?? "";
        assert.ok(
          data.includes(targets[i].expect),
          `[${targets[i].encoding.id}] expected string "${targets[i].expect}" not found in output: ${JSON.stringify(data)}`,
        );
      }

      // Also confirm nothing crossed over (no other tab's string leaked in)
      for (let i = 0; i < targets.length; i++) {
        const data = buffers.get(terminals[i]) ?? "";
        for (let j = 0; j < targets.length; j++) {
          if (i === j) continue;
          assert.ok(
            !data.includes(targets[j].expect),
            `[${targets[i].encoding.id}]'s terminal contains a string meant for [${targets[j].encoding.id}]`,
          );
        }
      }
    } finally {
      sub.dispose();
      for (const t of terminals) t.dispose();
    }
  });
});
