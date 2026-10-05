/**
 * Gets the list of locales actually generated on the host.
 *
 * `resolveLocaleEnv()` in `locale.ts` checks whether a given region+charset
 * combination actually exists on the host against the list obtained here.
 * The result of running `locale -a` is returned as a plain list of strings;
 * normalization and matching are the caller's (locale.ts's) responsibility.
 */
import { execFileSync } from "child_process";

/**
 * Gets the output of `locale -a`.
 *
 * - On success: an array of locale names (an array even when empty).
 * - On failure (e.g. an environment without the command): `null`.
 *
 * Not memoized: it runs once per terminal and takes milliseconds, and a
 * locale the user generates after being told one is missing should be
 * picked up by the next terminal without reloading the window.
 */
export function listAvailableLocales(): readonly string[] | null {
  try {
    const out = execFileSync("locale", ["-a"], {
      encoding: "utf8",
      timeout: 2000,
    });
    return out
      .split("\n")
      .map((line) => line.trim())
      .filter((line) => line.length > 0);
  } catch {
    return null;
  }
}
