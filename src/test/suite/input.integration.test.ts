import * as assert from "node:assert";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as vscode from "vscode";
import { ENCODINGS, EncodingDefinition } from "../../encodings";
import type { TestExports } from "../../extension";
import { useTestShell } from "./testShell";
import { waitFor } from "./waitFor";

/**
 * Input through VS Code's own paths, as the user gives it: a paste by VS
 * Code's paste command, which wraps it in bracketed paste markers when the
 * program turned that on, and keys sent after the Escape key. The "shell"
 * is a script that turns bracketed paste on and records the bytes it gets
 * in raw mode, so what VS Code and the converter hand the program is
 * checked byte for byte, without a line editor in between.
 */
suite(
  "input through VS Code reaches the program as the encoding's bytes",
  function () {
    this.timeout(30000);

    let dir: string;
    let recorded: string;
    let restoreShell: () => Promise<void>;
    let exports: TestExports;
    let extensionPath: string;

    suiteSetup(async () => {
      const ext = vscode.extensions.getExtension<TestExports>(
        "yutotnh.terminal-any-encoding",
      );
      exports = await ext!.activate();
      extensionPath = ext!.extensionPath;
      dir = fs.mkdtempSync(path.join(os.tmpdir(), "tae-input-"));
      recorded = path.join(dir, "recorded");
      const script = path.join(dir, "record");
      const lines = [
        "#!/bin/sh",
        "printf '\\033[?2004h'",
        "stty raw -echo",
        `exec cat > '${recorded}'`,
      ];
      fs.writeFileSync(script, lines.join("\n") + "\n");
      fs.chmodSync(script, 0o755);
      restoreShell = await useTestShell(script);
    });

    suiteTeardown(async () => {
      await restoreShell();
      fs.rmSync(dir, { recursive: true, force: true });
    });

    /** Opens an EUC-JP terminal on the recording script, runs steps, and
     * returns what the script got once it ends with `last` */
    async function record(
      steps: (terminal: vscode.Terminal) => Promise<void>,
      last: string,
    ): Promise<Buffer> {
      fs.rmSync(recorded, { force: true });
      const encoding = ENCODINGS.find((e) => e.id === "eucjp")!;
      const built = exports.buildTerminalOptions(extensionPath, encoding);
      assert.ok(built.ok);
      const terminal = vscode.window.createTerminal(built.options);
      try {
        terminal.show();
        await waitFor(() => fs.existsSync(recorded), 10000);
        await new Promise((r) => setTimeout(r, 500));
        await steps(terminal);
        await waitFor(
          () =>
            fs.existsSync(recorded) &&
            fs.readFileSync(recorded).toString("latin1").endsWith(last),
          5000,
        );
        return fs.readFileSync(recorded);
      } finally {
        terminal.dispose();
      }
    }

    const paste = async (text: string) => {
      await vscode.env.clipboard.writeText(text);
      await vscode.commands.executeCommand("workbench.action.terminal.paste");
      await new Promise((r) => setTimeout(r, 500));
    };

    test("a paste arrives bracketed and encoded; one with a character EUC-JP can't represent arrives empty and is reported", async () => {
      const rejected: EncodingDefinition[] = [];
      const sub = exports.onDidRejectInput((e) => rejected.push(e));
      try {
        const got = await record(async (terminal) => {
          await paste("echo あ");
          await paste("echo ☃");
          terminal.sendText("ok", false);
        }, "ok");
        const expected = Buffer.concat([
          Buffer.from("\x1b[200~echo ", "latin1"),
          Buffer.from([0xa4, 0xa2]), // あ in EUC-JP
          Buffer.from("\x1b[201~\x1b[200~\x1b[201~ok", "latin1"),
        ]);
        assert.strictEqual(got.toString("hex"), expected.toString("hex"));
        assert.deepStrictEqual(
          rejected.map((e) => e.id),
          ["eucjp"],
        );
      } finally {
        sub.dispose();
      }
    });

    test("a character typed after the Escape key arrives as itself", async () => {
      const got = await record(async (terminal) => {
        terminal.sendText("\x1b", false);
        await new Promise((r) => setTimeout(r, 300));
        terminal.sendText("あ", false);
        await new Promise((r) => setTimeout(r, 300));
        terminal.sendText("ok", false);
      }, "ok");
      const expected = Buffer.from([0x1b, 0xa4, 0xa2, 0x6f, 0x6b]);
      assert.strictEqual(got.toString("hex"), expected.toString("hex"));
    });
  },
);
