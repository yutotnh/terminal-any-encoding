import * as assert from "node:assert";
import * as vscode from "vscode";
import { ENCODINGS } from "../../encodings";
import type { TestExports } from "../../extension";
import { useTestShell } from "./testShell";

const EXTENSION_ID = "yutotnh.terminal-any-encoding";

/**
 * LANG is applied inside the transcoder (`env LANG=... <shell>`), so
 * terminal.integrated.detectLocale (which rewrites LANG of the process VS
 * Code launches, i.e. the transcoder) can't override it. /bin/sh is used so
 * no rc file of the machine running the tests changes LANG afterwards.
 */
suite("locale reaches the shell", function () {
  this.timeout(30000);

  test("detectLocale \"on\" doesn't override the encoding's LANG", async function () {
    const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
    const exports = await ext!.activate();
    const encoding = ENCODINGS.find((e) => e.id === "eucjp")!;
    const terminalConfig = () =>
      vscode.workspace.getConfiguration("terminal.integrated");

    await terminalConfig().update(
      "detectLocale",
      "on",
      vscode.ConfigurationTarget.Global,
    );
    const restoreShell = await useTestShell("/bin/sh");
    let terminal: vscode.Terminal | undefined;
    let output = "";
    const sub = vscode.window.onDidWriteTerminalData((e) => {
      if (e.terminal === terminal) output += e.data;
    });
    try {
      const built = exports.buildTerminalOptions(ext!.extensionPath, encoding);
      assert.ok(built.ok);
      if (built.locale.kind !== "matched" || built.locale.conflicts.length) {
        // No EUC-JP locale on this host, or an LC_* in the test runner's
        // environment outranks LANG; nothing to verify here.
        this.skip();
      }
      const locale = built.locale.locale;

      terminal = vscode.window.createTerminal(built.options);
      await new Promise((r) => setTimeout(r, 1500));
      terminal.sendText('echo "LOC=[$LANG|$(locale charmap)]"', true);
      // glibc says "EUC-JP", macOS "eucJP"
      const expected = new RegExp(
        `LOC=\\[${locale.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\|(EUC-JP|eucJP)\\]`,
      );
      const deadline = Date.now() + 15000;
      while (!expected.test(output) && Date.now() < deadline) {
        await new Promise((r) => setTimeout(r, 200));
      }
      assert.ok(
        expected.test(output),
        `expected ${expected} in ${JSON.stringify(output)}`,
      );
    } finally {
      sub.dispose();
      terminal?.dispose();
      await terminalConfig().update(
        "detectLocale",
        undefined,
        vscode.ConfigurationTarget.Global,
      );
      await restoreShell();
    }
  });
});
