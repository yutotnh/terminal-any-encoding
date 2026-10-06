/**
 * Polls until predicate holds or timeoutMs passes; resolves with whether it
 * held, so a test can wait for everything it checks and then assert, and a
 * timeout still fails with the assertion's message.
 */
export function waitFor(
  predicate: () => boolean,
  timeoutMs: number,
  intervalMs = 200,
): Promise<boolean> {
  return new Promise((resolve) => {
    const start = Date.now();
    const tick = () => {
      if (predicate()) return resolve(true);
      if (Date.now() - start > timeoutMs) return resolve(false);
      setTimeout(tick, intervalMs);
    };
    tick();
  });
}
