import * as assert from "node:assert";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import {
  installTranscoderCopy,
  pruneTranscoderCopies,
  resolveTranscoder,
  shellShim,
} from "../transcoder";

function makeTempDir(): string {
  return fs.mkdtempSync(path.join(os.tmpdir(), "tmes-test-"));
}

test("uses the bundled binary if it's executable", () => {
  const dir = makeTempDir();
  const binDir = path.join(dir, "transcoder", "bin");
  fs.mkdirSync(binDir, { recursive: true });
  const binPath = path.join(binDir, "luit");
  fs.writeFileSync(binPath, "#!/bin/sh\necho ok\n");
  fs.chmodSync(binPath, 0o755);

  const result = resolveTranscoder(dir);
  assert.strictEqual(result.ok, true);
  assert.strictEqual(result.location?.path, binPath);
});

test("returns the expected path when there's no bundled binary", () => {
  const dir = makeTempDir();
  const result = resolveTranscoder(dir);
  assert.strictEqual(result.ok, false);
  // Building the display string (localization) is the vscode-dependent
  // extension.ts's job, so only structured data is verified here.
  assert.strictEqual(
    result.missingPath,
    path.join(dir, "transcoder", "bin", "luit"),
  );
});

test("installTranscoderCopy: one copy per content, never overwritten, so each terminal keeps the transcoder it was created with", () => {
  const dir = makeTempDir();
  const bundled = path.join(dir, "bundled-luit");
  const storage = path.join(dir, "storage");
  fs.writeFileSync(bundled, "v1");
  fs.chmodSync(bundled, 0o755);

  const v1 = installTranscoderCopy(bundled, storage)!;
  assert.match(path.relative(storage, v1), /^[0-9a-f]{16}\/luit$/);
  assert.strictEqual(fs.readFileSync(v1, "utf8"), "v1");
  assert.ok(fs.statSync(v1).mode & 0o111);
  assert.strictEqual(installTranscoderCopy(bundled, storage), v1);

  // An update gets its own copy; v1's stays for terminals created with it,
  // and going back to v1 finds it again.
  fs.writeFileSync(bundled, "v2");
  const v2 = installTranscoderCopy(bundled, storage)!;
  assert.notStrictEqual(v2, v1);
  assert.strictEqual(fs.readFileSync(v1, "utf8"), "v1");
  fs.writeFileSync(bundled, "v1");
  assert.strictEqual(installTranscoderCopy(bundled, storage), v1);
  assert.strictEqual(
    fs.readdirSync(storage).filter((e) => !e.startsWith(".")).length,
    2,
  );
});

test("installTranscoderCopy: returns undefined when it can't copy (the caller falls back to the bundled path)", () => {
  const dir = makeTempDir();
  assert.strictEqual(
    installTranscoderCopy(path.join(dir, "missing"), path.join(dir, "s")),
    undefined,
  );
});

test("pruneTranscoderCopies: removes copies unused for too long (and leftovers), never the one in use", () => {
  const dir = makeTempDir();
  const storage = path.join(dir, "storage");
  const bundled = path.join(dir, "bundled-luit");
  fs.writeFileSync(bundled, "old");
  const old = installTranscoderCopy(bundled, storage)!;
  fs.writeFileSync(bundled, "recent");
  const recent = installTranscoderCopy(bundled, storage)!;
  fs.writeFileSync(bundled, "current");
  const current = installTranscoderCopy(bundled, storage)!;
  fs.mkdirSync(path.join(storage, ".tmp-abc"));
  fs.mkdirSync(path.join(storage, "unrelated"));

  const day = 24 * 60 * 60 * 1000;
  const now = Date.now();
  const age = (p: string, days: number) => {
    const t = new Date(now - days * day);
    fs.utimesSync(path.dirname(p), t, t);
  };
  age(old, 100);
  age(recent, 10);
  age(current, 1000); // in use: kept whatever its age

  pruneTranscoderCopies(storage, current, 90 * day, now);
  assert.strictEqual(fs.existsSync(old), false);
  assert.ok(fs.existsSync(recent));
  assert.ok(fs.existsSync(current));
  assert.strictEqual(fs.existsSync(path.join(storage, ".tmp-abc")), false);
  assert.ok(fs.existsSync(path.join(storage, "unrelated")));
});

test("shellShim: a link named like the shell, pointing at the stable transcoder, reused once made", () => {
  const dir = makeTempDir();
  const stable = path.join(dir, "luit");
  fs.writeFileSync(stable, "bin");
  const shim = shellShim(stable, "/usr/local/bin/zsh");
  assert.strictEqual(shim, path.join(dir, "shims", "zsh"));
  assert.strictEqual(fs.readlinkSync(shim!), stable);
  assert.strictEqual(shellShim(stable, "/bin/zsh"), shim);
  assert.deepStrictEqual(fs.readdirSync(path.join(dir, "shims")), ["zsh"]);
});
