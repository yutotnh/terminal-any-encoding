import * as assert from "node:assert";
import * as vscode from "vscode";
import type { TestExports } from "../../extension";
import { findOnPath, useTestShell } from "./testShell";
import { waitFor } from "./waitFor";
import { ENCODINGS } from "../../encodings";

const EXTENSION_ID = "yutotnh.terminal-any-encoding";

/**
 * Confirms command decoration, cwd tracking, and "run recent command" work
 * in an environment where rc files are never edited.
 *
 * A real VS Code terminal (xterm.js) correctly responds to terminal
 * capability negotiation (DA1/DA2/XTGETTCAP, etc.), so fish — which got
 * stuck in a plain direct-PTY test — can be correctly verified here too.
 */
suite(
  "shell integration, set up by VS Code itself through the shell-named link",
  function () {
    this.timeout(60000);

    async function checkShellIntegration(
      shellPath: string,
      label: string,
    ): Promise<void> {
      const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
      const exports = await ext!.activate();
      const encoding = ENCODINGS.find((e) => e.id === "eucjp")!;

      const restoreShell = await useTestShell(shellPath);

      let buffer = "";
      const built = exports.buildTerminalOptions(ext!.extensionPath, encoding);
      assert.strictEqual(
        built.ok,
        true,
        `${label}: failed to build terminal options`,
      );
      if (!built.ok) return;

      const terminal = vscode.window.createTerminal(built.options);
      const sub = vscode.window.onDidWriteTerminalData((e) => {
        if (e.terminal === terminal) buffer += e.data;
      });

      try {
        await new Promise((r) => setTimeout(r, 1500));
        terminal.sendText("echo shellintegtest", true);

        // Seeing OSC 633;A (prompt start) is proof shell integration is
        // active. The command's own marks (C, D) come after its echo, so
        // wait for all of them; the asserts below say which one is missing.
        await waitFor(
          () =>
            ["A", "B", "C", "D"].every((mark) =>
              buffer.includes(`\x1b]633;${mark}`),
            ) && buffer.includes("shellintegtest"),
          20000,
        );

        // Detecting OSC 633 inherently needs a regex that includes control characters (ESC/BEL)
        /* eslint-disable no-control-regex */
        assert.match(
          buffer,
          /\x1b\]633;A\x07/,
          `${label}: no OSC 633;A (prompt detection)`,
        );
        assert.match(
          buffer,
          /\x1b\]633;B\x07/,
          `${label}: no OSC 633;B (command start)`,
        );
        assert.match(
          buffer,
          /\x1b\]633;C\x07/,
          `${label}: no OSC 633;C (command execution)`,
        );
        assert.match(
          buffer,
          /\x1b\]633;D/,
          `${label}: no OSC 633;D (command end)`,
        );
        /* eslint-enable no-control-regex */
        assert.ok(
          buffer.includes("shellintegtest"),
          `${label}: command output not found`,
        );
      } finally {
        sub.dispose();
        terminal.dispose();
        await restoreShell();
      }
    }

    // fish and zsh are optional (CI installs them; a contributor's machine
    // may not have them), so those cases skip when the shell isn't on PATH.
    for (const [name, label] of [
      ["bash", "the full OSC 633 cycle (A/B/C/D) is emitted"],
      ["fish", "the full OSC 633 cycle (A/B/C/D) is emitted"],
      ["zsh", "the full OSC 633 cycle (A/B/C/D) is emitted (via ZDOTDIR)"],
    ] as const) {
      test(`${name}: ${label}`, async function () {
        // VS Code itself only injects into fish in newer versions (1.73's
        // pty host has no fish case; 1.85's does), and this extension
        // leaves injection to VS Code, exactly like a regular fish terminal.
        const [major, minor] = vscode.version.split(".").map(Number);
        if (name === "fish" && major === 1 && minor < 85) this.skip();
        const shellPath = findOnPath(name);
        if (!shellPath) {
          if (name === "bash") assert.fail("bash not found on PATH");
          this.skip();
        }
        await checkShellIntegration(shellPath!, name);
      });
    }

    test("terminal.integrated.shellIntegration.enabled=false: VS Code injects nothing, as for its own terminals", async () => {
      const ext = vscode.extensions.getExtension<TestExports>(EXTENSION_ID);
      const exports = await ext!.activate();
      const encoding = ENCODINGS.find((e) => e.id === "eucjp")!;
      const terminalConfig = () =>
        vscode.workspace.getConfiguration("terminal.integrated");
      const bashPath = findOnPath("bash");
      if (!bashPath) assert.fail("bash not found on PATH");

      const restoreShell = await useTestShell(bashPath!);
      await terminalConfig().update(
        "shellIntegration.enabled",
        false,
        vscode.ConfigurationTarget.Global,
      );
      let buffer = "";
      let terminal: vscode.Terminal | undefined;
      const sub = vscode.window.onDidWriteTerminalData((e) => {
        if (e.terminal === terminal) buffer += e.data;
      });
      try {
        const built = exports.buildTerminalOptions(
          ext!.extensionPath,
          encoding,
        );
        assert.ok(built.ok);
        terminal = vscode.window.createTerminal(built.options);
        await new Promise((r) => setTimeout(r, 1500));
        terminal.sendText("echo no-shell-integration", true);
        await waitFor(() => buffer.includes("no-shell-integration"), 10000);
        assert.ok(
          buffer.includes("no-shell-integration"),
          JSON.stringify(buffer),
        );
        await new Promise((r) => setTimeout(r, 500));
        assert.ok(!buffer.includes("\x1b]633;"), JSON.stringify(buffer));
      } finally {
        sub.dispose();
        terminal?.dispose();
        await terminalConfig().update(
          "shellIntegration.enabled",
          undefined,
          vscode.ConfigurationTarget.Global,
        );
        await restoreShell();
      }
    });
  },
);
