import * as assert from "node:assert";
import * as vscode from "vscode";
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

const EXTENSION_ID = "yutotnh.terminal-any-encoding";

suite("Extension activation", () => {
  test("the extension is found and can be activated", async () => {
    const ext = vscode.extensions.getExtension(EXTENSION_ID);
    assert.ok(ext, `Extension ${EXTENSION_ID} not found`);
    await ext!.activate();
    assert.strictEqual(ext!.isActive, true);
  });

  test("commands are registered", async () => {
    const commands = await vscode.commands.getCommands(true);
    assert.ok(
      commands.includes("terminalAnyEncoding.openTerminal"),
      "openTerminal command not found",
    );
    assert.ok(
      commands.includes("terminalAnyEncoding.openDefaultEncodingTerminal"),
      "openDefaultEncodingTerminal command not found",
    );
  });

  test("settings are registered", () => {
    const config = vscode.workspace.getConfiguration("terminalAnyEncoding");
    // Confirms the default is returned even when unset (proof contributes.configuration took effect)
    assert.strictEqual(config.get<boolean>("warnAboutLocale"), true);
  });
});

async function waitForOpenedTerminal(
  action: () => Thenable<unknown>,
): Promise<vscode.Terminal> {
  const opened = new Promise<vscode.Terminal>((resolve) => {
    const disposable = vscode.window.onDidOpenTerminal((terminal) => {
      disposable.dispose();
      resolve(terminal);
    });
  });
  await action();
  return opened;
}

suite("openTerminal: always shows the QuickPick", function () {
  this.timeout(15000);

  test("with no command argument, shows the QuickPick even when defaultEncoding is set, and doesn't open a terminal immediately", async () => {
    const config = vscode.workspace.getConfiguration("terminalAnyEncoding");
    await config.update(
      "defaultEncoding",
      "shiftjis",
      vscode.ConfigurationTarget.Global,
    );
    let opened = false;
    const disposable = vscode.window.onDidOpenTerminal(() => {
      opened = true;
    });
    try {
      // The QuickPick can't be auto-selected from a test, so instead of
      // awaiting the command's completion, confirm no terminal opens
      // within a fixed window.
      void vscode.commands.executeCommand("terminalAnyEncoding.openTerminal");
      await new Promise((r) => setTimeout(r, 800));
      assert.strictEqual(
        opened,
        false,
        "a terminal opened without going through the QuickPick, even though defaultEncoding is set",
      );
    } finally {
      disposable.dispose();
      await vscode.commands.executeCommand("workbench.action.closeQuickOpen");
      await config.update(
        "defaultEncoding",
        undefined,
        vscode.ConfigurationTarget.Global,
      );
    }
  });

  test("passing a command argument opens with that encoding, skipping the QuickPick", async () => {
    const terminal = await waitForOpenedTerminal(() =>
      vscode.commands.executeCommand(
        "terminalAnyEncoding.openTerminal",
        "eucjp",
      ),
    );
    assertTranscoderTerminal(terminal, "euc-jp-2007");
    terminal.dispose();
  });

  test("passing an invalid encoding id as an argument errors and doesn't open a terminal", async () => {
    let opened = false;
    const disposable = vscode.window.onDidOpenTerminal(() => {
      opened = true;
    });
    try {
      await vscode.commands.executeCommand(
        "terminalAnyEncoding.openTerminal",
        "no-such-encoding",
      );
      // showErrorMessage isn't modal, so control returns immediately.
      // Just confirm no terminal was created.
      assert.strictEqual(opened, false);
    } finally {
      disposable.dispose();
    }
  });
});

suite(
  "openDefaultEncodingTerminal: opens immediately, no QuickPick",
  function () {
    this.timeout(15000);

    test("opens immediately using defaultEncoding when it's set", async () => {
      const config = vscode.workspace.getConfiguration("terminalAnyEncoding");
      await config.update(
        "defaultEncoding",
        "eucjp",
        vscode.ConfigurationTarget.Global,
      );
      try {
        const terminal = await waitForOpenedTerminal(() =>
          vscode.commands.executeCommand(
            "terminalAnyEncoding.openDefaultEncodingTerminal",
          ),
        );
        assertTranscoderTerminal(terminal, "euc-jp-2007");
        terminal.dispose();
      } finally {
        await config.update(
          "defaultEncoding",
          undefined,
          vscode.ConfigurationTarget.Global,
        );
      }
    });

    test("shows the QuickPick when defaultEncoding is empty, and opens what's picked", async () => {
      // Nothing recently used, so the first entry is Windows 1252
      await (
        await vscode.extensions
          .getExtension<TestExports>("yutotnh.terminal-any-encoding")!
          .activate()
      ).clearRecentEncodings();
      await vscode.workspace
        .getConfiguration("terminalAnyEncoding")
        .update(
          "defaultEncoding",
          undefined,
          vscode.ConfigurationTarget.Global,
        );
      const terminal = await acceptFirstItemUntilTerminalOpens(() => {
        void vscode.commands.executeCommand(
          "terminalAnyEncoding.openDefaultEncodingTerminal",
        );
      });
      assertTranscoderTerminal(terminal, "CP1252");
      terminal.dispose();
    });
  },
);

suite("the picker lists recently used encodings first", function () {
  this.timeout(15000);

  test("after opening EUC-JP, it's the first entry", async () => {
    const exports = await vscode.extensions
      .getExtension<TestExports>("yutotnh.terminal-any-encoding")!
      .activate();
    await exports.clearRecentEncodings();
    const first = await waitForOpenedTerminal(() =>
      vscode.commands.executeCommand(
        "terminalAnyEncoding.openTerminal",
        "eucjp",
      ),
    );
    // Opening (and closing) a terminal moves the focus once it's ready,
    // which closes a QuickPick that has just opened: let it settle first,
    // and close it only at the end.
    await new Promise((r) => setTimeout(r, 2000));
    const second = await acceptFirstItemUntilTerminalOpens(() => {
      void vscode.commands.executeCommand("terminalAnyEncoding.openTerminal");
    });
    const transcoderArgs = (
      (second.creationOptions as vscode.TerminalOptions).env
        ?.TERMINAL_ANY_ENCODING_ARGS ?? ""
    ).split("\n");
    assert.ok(
      transcoderArgs.includes("euc-jp-2007"),
      JSON.stringify(transcoderArgs),
    );
    second.dispose();
    first.dispose();
  });
});
