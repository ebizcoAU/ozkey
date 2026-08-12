/**
 * LockSim's version string — DERIVED, never typed twice.
 *
 * `bridge32-1.32` shipped a hardcoded version badge on its LCD that drifted
 * from `FW_VERSION` and told the operator the wrong thing on the bench. The
 * fix there was to derive the badge from the one constant that already
 * existed; this is the same fix applied before it can happen here.
 *
 * `package.json` is the single source of truth. Bump it and everything
 * follows — the header, the browser tab, and anything else that asks.
 */

import pkg from "../package.json";

/**
 * Rendered as `V1.00`, matching the firmware's `doorlock-1.58` /
 * `bridge32-1.34` convention rather than bare semver — the operator reads all
 * three side by side on the bench, so they should look alike.
 *
 * major.minor only: the patch field is not shown, so a display-only change
 * does not need a version the firmware would have to match.
 */
export function formatVersion(semver: string): string {
  // Destructuring defaults are not enough here: `"".split(".")` is `[""]`, not
  // `[]`, so `major` binds to the empty string and the default never fires.
  const parts = semver.split(".");
  const major = parts[0] || "0";
  const minor = parts[1] || "0";
  return `V${major}.${minor.padStart(2, "0")}`;
}

export const LOCKSIM_VERSION = formatVersion(pkg.version);
