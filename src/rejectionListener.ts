/**
 * Receives the transcoder's reports of rejected input, so the extension can
 * tell the user why their input wasn't sent; the terminal bell alone is
 * silent with VS Code's default settings.
 *
 * Every extension host (≈ every VS Code window) listens on its own Unix
 * socket, `<pid>.sock`, in one per-user directory. The transcoder (luit's
 * -notify) sends each of them "unencodable <encoding> <pid> <hex code
 * point>", where pid is the one VS Code knows the terminal by, and the
 * caller keeps only reports from its own terminals.
 * Broadcasting to a fixed directory keeps reports flowing for terminals
 * revived after a window reload, whose extension host is a new one.
 */
import * as fs from "fs";
import * as net from "net";
import * as os from "os";
import * as path from "path";

export interface RejectionReport {
  readonly luitEncoding: string;
  readonly pid: number;
  /** The first character that couldn't be encoded, when reported */
  readonly character?: string;
}

function characterOf(hex: string | undefined): string | undefined {
  if (!hex) return undefined;
  const codePoint = parseInt(hex, 16);
  return codePoint > 0 && codePoint <= 0x10ffff
    ? String.fromCodePoint(codePoint)
    : undefined;
}

function userTag(): string {
  try {
    return os.userInfo().username;
  } catch {
    return String(process.getuid?.() ?? "user");
  }
}

/**
 * The per-user directory the sockets live in. The name is predictable and
 * the tmpdir shared, so an existing one is only used if it's a real
 * directory (not a symlink) owned by this user and closed to others.
 */
export function prepareNotifyDir(
  tmpDir: string = os.tmpdir(),
): string | undefined {
  const dir = path.join(tmpDir, `${userTag()}-terminal-any-encoding`);
  try {
    try {
      fs.mkdirSync(dir, { mode: 0o700 });
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code !== "EEXIST") throw e;
    }
    const st = fs.lstatSync(dir);
    if (!st.isDirectory()) return undefined;
    if (process.getuid && st.uid !== process.getuid()) return undefined;
    if ((st.mode & 0o077) !== 0) return undefined;
    return dir;
  } catch {
    return undefined;
  }
}

export class RejectionListener {
  private server: net.Server | undefined;
  private socketPath: string | undefined;

  constructor(private readonly onReport: (report: RejectionReport) => void) {}

  /** Starts listening in dir; returns false if it can't */
  async start(dir: string): Promise<boolean> {
    const socketPath = path.join(dir, `${process.pid}.sock`);
    try {
      // A leftover from a crashed host that had the same pid
      fs.rmSync(socketPath, { force: true });
      const server = net.createServer((socket) => {
        let pending = "";
        socket.setEncoding("utf8");
        socket.on("data", (chunk: string) => {
          pending += chunk;
          let newline: number;
          while ((newline = pending.indexOf("\n")) >= 0) {
            const line = pending.slice(0, newline);
            pending = pending.slice(newline + 1);
            const match = /^unencodable (\S+) (\d+)(?: ([0-9A-Fa-f]+))?$/.exec(
              line,
            );
            if (match) {
              const character = characterOf(match[3]);
              this.onReport({
                luitEncoding: match[1],
                pid: Number(match[2]),
                ...(character ? { character } : {}),
              });
            }
          }
          // A well-behaved transcoder never sends long lines
          if (pending.length > 1024) socket.destroy();
        });
        socket.on("error", () => socket.destroy());
      });
      await new Promise<void>((resolve, reject) => {
        server.once("error", reject);
        server.listen(socketPath, () => resolve());
      });
      this.server = server;
      this.socketPath = socketPath;
      return true;
    } catch {
      this.dispose();
      return false;
    }
  }

  dispose(): void {
    this.server?.close();
    this.server = undefined;
    if (this.socketPath) fs.rmSync(this.socketPath, { force: true });
    this.socketPath = undefined;
  }
}
