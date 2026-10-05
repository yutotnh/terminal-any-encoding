import * as assert from "node:assert";
import * as fs from "node:fs";
import * as net from "node:net";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import {
  RejectionListener,
  RejectionReport,
  prepareNotifyDir,
} from "../rejectionListener";

function send(socketPath: string, data: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const client = net.createConnection(socketPath, () => {
      client.end(data, () => resolve());
    });
    client.on("error", reject);
  });
}

function makeTmpDir(): string {
  return fs.mkdtempSync(path.join(os.tmpdir(), "tae-rl-"));
}

test("reports each 'unencodable <encoding> <pid> [<hex code point>]' line, also when lines arrive split or together", async () => {
  const seen: RejectionReport[] = [];
  const listener = new RejectionListener((r) => seen.push(r));
  const dir = prepareNotifyDir(makeTmpDir())!;
  assert.ok(await listener.start(dir));
  const socketPath = path.join(dir, `${process.pid}.sock`);
  try {
    await send(
      socketPath,
      ["unencodable euc-jp-2007 12", "unencodable CP9"].join("\n"),
    );
    await send(socketPath, "unencodable CP932 34 1F600\ngarbage\n");
    const deadline = Date.now() + 2000;
    while (seen.length < 2 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }
    assert.deepStrictEqual(seen, [
      { luitEncoding: "euc-jp-2007", pid: 12 },
      { luitEncoding: "CP932", pid: 34, character: "😀" },
    ]);
  } finally {
    listener.dispose();
  }
  assert.strictEqual(fs.existsSync(socketPath), false);
});

test("prepareNotifyDir: a private per-user directory; refuses a symlink or one open to others", () => {
  const tmp = makeTmpDir();
  const dir = prepareNotifyDir(tmp)!;
  assert.strictEqual(fs.statSync(dir).mode & 0o077, 0);
  assert.strictEqual(prepareNotifyDir(tmp), dir);

  const open = makeTmpDir();
  fs.mkdirSync(path.join(open, path.basename(dir)));
  fs.chmodSync(path.join(open, path.basename(dir)), 0o777);
  assert.strictEqual(prepareNotifyDir(open), undefined);

  const linked = makeTmpDir();
  fs.symlinkSync(makeTmpDir(), path.join(linked, path.basename(dir)));
  assert.strictEqual(prepareNotifyDir(linked), undefined);
});
