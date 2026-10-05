import * as assert from "node:assert";
import * as vscode from "vscode";

/**
 * Nothing else runs in this window (see index.ts), so only the onTaskType
 * activation event can activate the extension here. It's listed explicitly
 * in package.json because 1.73 doesn't derive it from taskDefinitions. The
 * test workspace has a terminalAnyEncoding task in its tasks.json (see
 * runTest.ts).
 */
suite("onTaskType activation", () => {
  test("fetching tasks activates the extension, so tasks.json tasks resolve", async () => {
    const ext = vscode.extensions.getExtension("yutotnh.terminal-any-encoding");
    assert.ok(ext);
    assert.strictEqual(ext!.isActive, false, "already active before fetching");
    await vscode.tasks.fetchTasks();
    assert.strictEqual(ext!.isActive, true);
  });
});
