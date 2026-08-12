/**
 * Filesystem loading for `profiles/`. Split from `profile.ts` so the resolver
 * stays pure and runs in the browser — only this module touches `node:fs`.
 */

import { readFileSync, readdirSync } from "node:fs";
import { join, dirname, resolve as resolvePath } from "node:path";
import { fileURLToPath } from "node:url";
import {
  type Catalogue,
  type ProductProfile,
  type ResolvedProfile,
  ProfileError,
  resolveProfile,
} from "./profile";

/** Repo-root `profiles/`, from `locksim/lib/` — the profiles are SHARED with the firmware. */
export const PROFILES_DIR = resolvePath(
  dirname(fileURLToPath(import.meta.url)),
  "..",
  "..",
  "profiles"
);

/**
 * Strip `$`-prefixed keys recursively.
 *
 * The profile JSON is heavily commented via `$comment` — that is deliberate,
 * because a DP table that says "🔴 our fiction used DP 21 for add_temp_pin" is
 * worth far more to the next reader than a clean file. But documentation must
 * not reach the runtime model: `in_lock` picked up a `$comment` key and
 * `Object.keys()` then reported a bond DPID that does not exist.
 */
function stripComments<T>(value: T): T {
  if (Array.isArray(value)) return value.map(stripComments) as unknown as T;
  if (value && typeof value === "object") {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
      if (k.startsWith("$")) continue;
      out[k] = stripComments(v);
    }
    return out as T;
  }
  return value;
}

function readJson<T>(path: string): T {
  try {
    return stripComments(JSON.parse(readFileSync(path, "utf8")) as T);
  } catch (e) {
    throw new ProfileError(`${path}: ${(e as Error).message}`);
  }
}

export function loadCatalogue(id: string, dir = PROFILES_DIR): Catalogue {
  return readJson<Catalogue>(join(dir, `${id}.json`));
}

export function loadProduct(id: string, dir = PROFILES_DIR): ProductProfile {
  return readJson<ProductProfile>(join(dir, "products", `${id}.json`));
}

export function listProducts(dir = PROFILES_DIR): string[] {
  return readdirSync(join(dir, "products"))
    .filter((f) => f.endsWith(".json"))
    .map((f) => f.replace(/\.json$/, ""))
    .sort();
}

/** Load a product profile and merge its catalogue. */
export function loadProfile(id: string, dir = PROFILES_DIR): ResolvedProfile {
  const product = loadProduct(id, dir);
  const catalogue =
    product.standalone || !product.catalogue ? undefined : loadCatalogue(product.catalogue, dir);
  return resolveProfile(product, catalogue);
}

/**
 * The profile LockSim boots with.
 *
 * 🔴 Deliberately the INVENTED map. `doorlock-1.58` ships it and the current
 * BANOI build constructs DP 21 frames against it, so defaulting to the real
 * catalogue would break DoorA/DoorB and the shipping app on the next flash.
 * Staged migration, no flag day — `ozkey-28 §1.1`.
 */
export const DEFAULT_PROFILE_ID = "ozkie-legacy-v0";
