import * as vscode from "vscode";

/**
 * Accepts the active item of the QuickPick a just-started action shows,
 * repeatedly until it has opened a terminal: the QuickPick takes a moment
 * to appear (longer on old VS Code versions), and accepting before that
 * does nothing.
 */
export async function acceptFirstItemUntilTerminalOpens(
  start: () => void,
  timeoutMs = 10000,
): Promise<vscode.Terminal> {
  let opened: vscode.Terminal | undefined;
  const sub = vscode.window.onDidOpenTerminal((t) => (opened ??= t));
  try {
    start();
    const deadline = Date.now() + timeoutMs;
    while (!opened && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 500));
      if (!opened) {
        await vscode.commands.executeCommand(
          "workbench.action.acceptSelectedQuickOpenItem",
        );
      }
    }
    if (!opened) throw new Error("no terminal opened from the QuickPick");
    return opened;
  } finally {
    sub.dispose();
  }
}
