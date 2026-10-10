/**
 * Locates the bundled transcoder (luit fork) and manages the copies and
 * links terminals launch it through.
 *
 * Each per-platform VSIX bundles the binary for its own platform at
 * `transcoder/bin/luit` (see release.yml), so there's nothing to detect at
 * runtime. Windows isn't supported, so there's no `.exe`.
 */
import * as crypto from "crypto";
import * as fs from "fs";
import * as path from "path";

export type TranscoderResolution =
  | { readonly ok: true; readonly path: string }
  /** The path the binary should be at; the caller builds the message */
  | { readonly ok: false; readonly missingPath: string };

export function isExecutableFile(p: string): boolean {
  try {
    const st = fs.statSync(p);
    if (!st.isFile()) return false;
    fs.accessSync(p, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * The bundled transcoder in the extension's install directory
 * (context.extensionPath)
 */
export function resolveTranscoder(extensionPath: string): TranscoderResolution {
  const bundled = path.join(extensionPath, "transcoder", "bin", "luit");
  return isExecutableFile(bundled)
    ? { ok: true, path: bundled }
    : { ok: false, missingPath: bundled };
}

/**
 * Keeps a copy of the transcoder at a path of its own that never changes
 * (`<storageDir>/<hash of its contents>/luit`), and returns that path, or
 * undefined if it can't (the caller then uses the bundled one).
 *
 * VS Code restores terminals after a restart by launching the saved
 * executable path with the saved environment, before any extension
 * activates. The bundled path contains the extension's version, so after an
 * update it would be gone. Copies are never overwritten, so each terminal
 * keeps running the very transcoder it was created with, whichever version
 * of the extension is installed by then: after an update, after going back
 * to an older version, or with VS Code windows that haven't reloaded into
 * the update yet. The options it was started with are always ones that
 * transcoder knows. Unused copies are removed by pruneTranscoderCopies().
 */
export function installTranscoderCopy(
  bundledPath: string,
  storageDir: string,
): string | undefined {
  try {
    const bundled = fs.readFileSync(bundledPath);
    const hash = crypto.createHash("sha256").update(bundled).digest("hex");
    const dir = path.join(storageDir, hash.slice(0, 16));
    const copy = path.join(dir, "luit");
    if (!isExecutableFile(copy)) {
      // Built aside and renamed into place, so another window doing the
      // same at the same moment never sees half a directory.
      fs.mkdirSync(storageDir, { recursive: true });
      const temp = fs.mkdtempSync(path.join(storageDir, ".tmp-"));
      fs.writeFileSync(path.join(temp, "luit"), bundled, { mode: 0o755 });
      try {
        fs.renameSync(temp, dir);
      } catch {
        fs.rmSync(temp, { recursive: true, force: true }); // someone else won
      }
    }
    if (!isExecutableFile(copy)) return undefined;
    // The transcoder marks its directory as used each time it starts (a
    // restored terminal included); this does it for a fresh one.
    const now = new Date();
    fs.utimesSync(dir, now, now);
    return copy;
  } catch {
    return undefined;
  }
}

const COPY_DIR = /^[0-9a-f]{16}$/;

/**
 * Removes transcoder copies (other than keep) that haven't been used for
 * maxAgeMs. A copy is "used" whenever a terminal starts from it, restored
 * ones included, so only copies no terminal has needed for that long go:
 * VS Code restores a terminal at every start, which keeps its copy alive.
 */
export function pruneTranscoderCopies(
  storageDir: string,
  keep: string | undefined,
  maxAgeMs: number,
  now: number = Date.now(),
): void {
  let entries: string[];
  try {
    entries = fs.readdirSync(storageDir);
  } catch {
    return;
  }
  const keepDir = keep ? path.dirname(keep) : undefined;
  for (const entry of entries) {
    const dir = path.join(storageDir, entry);
    if (dir === keepDir) continue;
    try {
      const stale =
        entry.startsWith(".tmp-") ||
        (COPY_DIR.test(entry) && now - fs.statSync(dir).mtimeMs > maxAgeMs);
      if (stale) fs.rmSync(dir, { recursive: true, force: true });
    } catch {
      // gone already, or not ours to judge
    }
  }
}

/**
 * A link to the transcoder named like the inner shell (`<dir of the
 * copy>/shims/<shell basename>`), or undefined if it can't be made.
 *
 * VS Code decides how to set up shell integration from the basename of the
 * executable it launches. Launched as ".../bash", the transcoder gets the
 * same treatment as a regular bash terminal: VS Code's own injection, nonce,
 * PATH fix-ups, environment reporting and shell type. The transcoder takes
 * its own options from an environment variable (see luit's
 * expandArgsFromEnv), since VS Code replaces the arguments when injecting.
 */
export function shellShim(
  stablePath: string,
  shellPath: string,
): string | undefined {
  const name = path.basename(shellPath);
  if (!name || name === "." || name === "..") return undefined;
  const dir = path.join(path.dirname(stablePath), "shims");
  const shim = path.join(dir, name);
  try {
    if (fs.readlinkSync(shim) === stablePath) return shim;
  } catch {
    // missing, or not a link: (re)create it below
  }
  try {
    fs.mkdirSync(dir, { recursive: true });
    const temp = `${shim}.${process.pid}.${Date.now()}`;
    fs.symlinkSync(stablePath, temp);
    fs.renameSync(temp, shim);
    return shim;
  } catch {
    return undefined;
  }
}
