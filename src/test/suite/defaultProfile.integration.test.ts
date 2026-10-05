import * as assert from "node:assert";
import * as vscode from "vscode";
import { getEncoding } from "../../encodings";

const PLATFORM = process.platform === "darwin" ? "osx" : "linux";
import type { TestExports } from "../../extension";
import { acceptFirstItemUntilTerminalOpens } from "./quickPick";

/** The terminal runs through the transcoder with the given encoding */
function assertTranscoderTerminal(
  terminal: vscode.Terminal | undefined,
  luitEncoding: string,
): void {
  assert.ok(terminal, "no terminal opened");
  // The transcoder's options travel in the environment (VS Code may
  // replace the arguments when it injects shell integration).
  const transcoderArgs = (
    (terminal.creationOptions as vscode.TerminalOptions).env
      ?.TERMINAL_ANY_ENCODING_ARGS ?? ""
  ).split("\n");
  assert.ok(
    transcoderArgs.includes(luitEncoding),
    `${luitEncoding} not in ${JSON.stringify(transcoderArgs)}`,
  );
}

suite("the dropdown profiles", function () {
  this.timeout(20000);

  // Right after VS Code starts, a defaultProfile naming a contributed
  // profile quietly opens the regular default shell instead: VS Code picks
  // up contributed profiles in refreshAvailableProfiles(), which is
  // debounced by 2 s, and new terminals only wait for profilesReady, which
  // opens once the detected shells are known. There's no API to tell when
  // the contributed ones are in, so open terminals through the Default
  // Encoding profile until one is ours. Without this, these tests fail most
  // of the time when they run first in a fresh window (on Xvfb too).
  suiteSetup(async function () {
    this.timeout(60000);
    const terminalConfig = vscode.workspace.getConfiguration(
      "terminal.integrated",
    );
    const extensionConfig = vscode.workspace.getConfiguration(
      "terminalAnyEncoding",
    );
    const original = terminalConfig.get(`defaultProfile.${PLATFORM}`);
    await extensionConfig.update(
      "defaultEncoding",
      "shiftjis",
      vscode.ConfigurationTarget.Global,
    );
    await terminalConfig.update(
      `defaultProfile.${PLATFORM}`,
      "🌐 Default Encoding",
      vscode.ConfigurationTarget.Global,
    );
    try {
      const deadline = Date.now() + 30000;
      for (;;) {
        const opened = new Promise<vscode.Terminal>((resolve) => {
          const disposable = vscode.window.onDidOpenTerminal((terminal) => {
            disposable.dispose();
            resolve(terminal);
          });
        });
        await vscode.commands.executeCommand("workbench.action.terminal.new");
        const terminal = await opened;
        const ours =
          (terminal.creationOptions as vscode.TerminalOptions).env
            ?.TERMINAL_ANY_ENCODING_ARGS !== undefined;
        terminal.dispose();
        if (ours) break;
        if (Date.now() > deadline) {
          throw new Error("VS Code never opened the contributed profile");
        }
        await new Promise((r) => setTimeout(r, 500));
      }
    } finally {
      await terminalConfig.update(
        `defaultProfile.${PLATFORM}`,
        original,
        vscode.ConfigurationTarget.Global,
      );
      await extensionConfig.update(
        "defaultEncoding",
        undefined,
        vscode.ConfigurationTarget.Global,
      );
    }
  });

  test('pointing defaultProfile.linux at "🌐 Default Encoding" makes the standard new-terminal command open defaultEncoding', async () => {
    const terminalConfig = vscode.workspace.getConfiguration(
      "terminal.integrated",
    );
    const extensionConfig = vscode.workspace.getConfiguration(
      "terminalAnyEncoding",
    );
    const original = terminalConfig.get(`defaultProfile.${PLATFORM}`);
    const sjis = getEncoding("shiftjis")!;
    await extensionConfig.update(
      "defaultEncoding",
      sjis.id,
      vscode.ConfigurationTarget.Global,
    );
    await terminalConfig.update(
      `defaultProfile.${PLATFORM}`,
      // The profile's title, as shown in the dropdown (with the 🌐 prefix)
      "🌐 Default Encoding",
      vscode.ConfigurationTarget.Global,
    );
    try {
      const opened = new Promise<vscode.Terminal>((resolve) => {
        const disposable = vscode.window.onDidOpenTerminal((terminal) => {
          disposable.dispose();
          resolve(terminal);
        });
      });
      // Must go through the same command path as the standard shortcut
      // (Ctrl+Shift+`), not the vscode.window.createTerminal() API.
      await vscode.commands.executeCommand("workbench.action.terminal.new");
      const terminal = await opened;
      // The tab names the encoding (on Linux it's the transcoder's process
      // title, so it can take a moment; spaces there are no-break spaces).
      const deadline = Date.now() + 5000;
      while (
        !/[ \u00a0]\(Shift[ \u00a0]JIS\)$/.test(terminal.name) &&
        Date.now() < deadline
      ) {
        await new Promise((r) => setTimeout(r, 100));
      }
      assert.match(terminal.name, /[ \u00a0]\(Shift[ \u00a0]JIS\)$/);
      const options = terminal.creationOptions as vscode.TerminalOptions;
      // The transcoder's options travel in the environment (VS Code may
      // replace the arguments when it injects shell integration).
      const transcoderArgs = (
        options.env?.TERMINAL_ANY_ENCODING_ARGS ?? ""
      ).split("\n");
      assert.ok(
        transcoderArgs.includes("CP932"),
        `CP932 not in ${JSON.stringify(transcoderArgs)}`,
      );
      // The default profile is this extension's own, so the inner shell
      // must come from the fallback ($SHELL), not loop back into it.
      assert.ok(!transcoderArgs.some((a) => a.includes("Default Encoding")));
      terminal.dispose();
    } finally {
      await terminalConfig.update(
        `defaultProfile.${PLATFORM}`,
        original,
        vscode.ConfigurationTarget.Global,
      );
      await extensionConfig.update(
        "defaultEncoding",
        undefined,
        vscode.ConfigurationTarget.Global,
      );
    }
  });

  test('"🌐 Select Encoding..." shows the QuickPick and opens what\'s picked; dismissing it opens nothing', async () => {
    // Launched the way the dropdown launches a contributed profile; going
    // through defaultProfile is the reliable way to do that from a test.
    const terminalConfig = vscode.workspace.getConfiguration(
      "terminal.integrated",
    );
    const original = terminalConfig.get(`defaultProfile.${PLATFORM}`);
    await terminalConfig.update(
      `defaultProfile.${PLATFORM}`,
      "🌐 Select Encoding...",
      vscode.ConfigurationTarget.Global,
    );
    const opened: vscode.Terminal[] = [];
    const disposable = vscode.window.onDidOpenTerminal((t) => opened.push(t));
    try {
      void vscode.commands.executeCommand("workbench.action.terminal.new");
      await new Promise((r) => setTimeout(r, 1000));
      await vscode.commands.executeCommand("workbench.action.closeQuickOpen");
      await new Promise((r) => setTimeout(r, 1000));
      assert.strictEqual(opened.length, 0, "dismissing opened a terminal");

      // Nothing recently used, so the first entry is Windows 1252
      await (
        await vscode.extensions
          .getExtension<TestExports>("yutotnh.terminal-any-encoding")!
          .activate()
      ).clearRecentEncodings();
      const terminal = await acceptFirstItemUntilTerminalOpens(() => {
        void vscode.commands.executeCommand("workbench.action.terminal.new");
      });
      opened.push(terminal);
      assertTranscoderTerminal(terminal, "CP1252");
    } finally {
      disposable.dispose();
      for (const t of opened) t.dispose();
      await terminalConfig.update(
        `defaultProfile.${PLATFORM}`,
        original,
        vscode.ConfigurationTarget.Global,
      );
    }
  });

  test("a terminal opened through the QuickPick becomes the active one, not the terminal that was active before", async () => {
    // The race this guards against (see showWhenOpened) only shows over a
    // remote connection; locally this pins that nothing else steals it.
    const terminalConfig = vscode.workspace.getConfiguration(
      "terminal.integrated",
    );
    const original = terminalConfig.get(`defaultProfile.${PLATFORM}`);
    const opened: vscode.Terminal[] = [];
    const disposable = vscode.window.onDidOpenTerminal((t) => opened.push(t));
    try {
      const previous = vscode.window.createTerminal("previous");
      previous.show();
      while (vscode.window.activeTerminal !== previous) {
        await new Promise((r) => setTimeout(r, 100));
      }
      await new Promise((r) => setTimeout(r, 1000));
      await terminalConfig.update(
        `defaultProfile.${PLATFORM}`,
        "🌐 Select Encoding...",
        vscode.ConfigurationTarget.Global,
      );
      const terminal = await acceptFirstItemUntilTerminalOpens(() => {
        void vscode.commands.executeCommand("workbench.action.terminal.new");
      });
      // Give VS Code time to settle on the active terminal
      await new Promise((r) => setTimeout(r, 2000));
      assert.strictEqual(vscode.window.activeTerminal, terminal);
    } finally {
      disposable.dispose();
      for (const t of opened) t.dispose();
      await terminalConfig.update(
        `defaultProfile.${PLATFORM}`,
        original,
        vscode.ConfigurationTarget.Global,
      );
    }
  });

  test("a dismissed QuickPick can cancel the profile request (so VS Code drops it quietly instead of showing an error)", async () => {
    // The extension host passes provideTerminalProfile a token from its
    // own CancellationTokenSource, the same class the API exposes; this
    // pins that its token still has the internal cancel() relied on.
    const ext = vscode.extensions.getExtension<TestExports>(
      "yutotnh.terminal-any-encoding",
    );
    const exports = await ext!.activate();
    const source = new vscode.CancellationTokenSource();
    exports.cancelProfileRequest(source.token);
    assert.strictEqual(source.token.isCancellationRequested, true);
    source.dispose();
  });
});
