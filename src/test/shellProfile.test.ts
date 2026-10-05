import * as assert from "node:assert";
import { test } from "node:test";
import {
  ShellProfileInput,
  resolveShell,
  shellCommandFlag,
} from "../shellProfile";

// VS Code's built-in profiles (terminal.integrated.profiles.*'s defaults)
const LINUX_PROFILES = {
  bash: { path: "bash" },
  zsh: { path: "zsh" },
  fish: { path: "fish" },
};
const OSX_PROFILES = {
  bash: { path: "bash", args: ["-l"] },
  zsh: { path: "zsh", args: ["-l"] },
};

function input(overrides: Partial<ShellProfileInput>): ShellProfileInput {
  return {
    shellProfileName: undefined,
    defaultProfileName: undefined,
    profiles: LINUX_PROFILES,
    platform: "linux",
    env: { SHELL: "/bin/bash" },
    homeDir: "/home/u",
    exists: () => true,
    ...overrides,
  };
}

test("no default profile: like VS Code, $SHELL with the matching profile's settings on Linux, a login shell on macOS", () => {
  // Linux: the "bash" profile matches $SHELL=/bin/bash
  assert.deepStrictEqual(resolveShell(input({})), {
    path: "/bin/bash",
    args: [],
    env: {},
  });
  assert.deepStrictEqual(
    resolveShell(
      input({
        profiles: {
          "my zsh": { path: "/usr/bin/zsh", args: ["-l"], env: { A: "1" } },
        },
        env: { SHELL: "/bin/zsh" },
      }),
    ),
    { path: "/bin/zsh", args: ["-l"], env: { A: "1" } },
  );
  // No profile with that name: $SHELL alone
  assert.deepStrictEqual(
    resolveShell(input({ env: { SHELL: "/usr/bin/nu" } })),
    { path: "/usr/bin/nu", args: [], env: {} },
  );
  assert.deepStrictEqual(
    resolveShell(
      input({
        platform: "osx",
        profiles: OSX_PROFILES,
        env: { SHELL: "/bin/zsh" },
      }),
    ).args,
    ["--login"],
  );
  assert.deepStrictEqual(
    resolveShell(input({ platform: "osx", env: { SHELL: "/bin/tcsh" } })).args,
    [],
  );
  assert.strictEqual(resolveShell(input({ env: {} })).path, "/bin/sh");
});

test("the default profile's path, args and env are used, with ${env:...}/${userHome} resolved", () => {
  const resolved = resolveShell(
    input({
      defaultProfileName: "mine",
      profiles: {
        mine: {
          path: "${userHome}/bin/zsh",
          args: ["-l", "--foo=${env:FOO}"],
          env: { A: "1", B: null, C: 3 },
        },
      },
      env: { FOO: "bar" },
    }),
  );
  assert.deepStrictEqual(resolved, {
    path: "/home/u/bin/zsh",
    args: ["-l", "--foo=bar"],
    env: { A: "1", B: null },
  });
});

test("macOS's built-in profiles carry -l", () => {
  const resolved = resolveShell(
    input({
      platform: "osx",
      defaultProfileName: "zsh",
      profiles: OSX_PROFILES,
    }),
  );
  assert.deepStrictEqual(resolved.args, ["-l"]);
  assert.strictEqual(resolved.path, "zsh");
});

test("a path array uses the first one that exists", () => {
  const resolved = resolveShell(
    input({
      defaultProfileName: "x",
      profiles: { x: { path: ["/nope/zsh", "/usr/bin/zsh"] } },
      exists: (p) => p === "/usr/bin/zsh",
    }),
  );
  assert.strictEqual(resolved.path, "/usr/bin/zsh");
});

test("falls back to $SHELL for a profile it can't run: another extension's (including this one's), a missing path, null, or an unknown name", () => {
  for (const profiles of [
    { x: { extensionIdentifier: "yutotnh.terminal-any-encoding", id: "d" } },
    { x: { path: "/nope" } },
    { x: null },
    {},
  ]) {
    const resolved = resolveShell(
      input({
        defaultProfileName: "x",
        profiles,
        exists: (p) => p !== "/nope",
      }),
    );
    assert.strictEqual(resolved.path, "/bin/bash", JSON.stringify(profiles));
  }
});

test("terminalAnyEncoding.shellProfile names the profile to use instead of the default one", () => {
  const resolved = resolveShell(
    input({
      shellProfileName: "fish",
      defaultProfileName: "zsh",
    }),
  );
  assert.strictEqual(resolved.path, "fish");
});

test("shellCommandFlag: the flag VS Code's own shell tasks pass before the command line", () => {
  for (const shell of ["/bin/bash", "zsh", "/usr/bin/fish", "sh", "nu"]) {
    assert.strictEqual(shellCommandFlag(shell), "-c", shell);
  }
  for (const shell of ["/usr/bin/pwsh", "pwsh-preview", "powershell"]) {
    assert.strictEqual(shellCommandFlag(shell), "-Command", shell);
  }
});
