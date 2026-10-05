import * as path from "path";
import Mocha from "mocha";

/**
 * A run of its own (see runTest.ts), so nothing else has activated the
 * extension or used its commands or profiles yet.
 */
export async function run(): Promise<void> {
  const mocha = new Mocha({ ui: "tdd", color: false, timeout: 30000 });
  mocha.addFile(path.resolve(__dirname, "onTaskType.test.js"));
  return new Promise((resolve, reject) => {
    mocha.run((failures) =>
      failures > 0
        ? reject(new Error(`${failures} test(s) failed`))
        : resolve(),
    );
  });
}
