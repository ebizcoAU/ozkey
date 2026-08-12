/**
 * Browser-safe profile registry.
 *
 * `profileLoad.ts` reads `profiles/` off disk and is used by the tests, which
 * is how we verify that every file on disk resolves. The BROWSER cannot do
 * that, so the same JSON is imported statically here and resolved at module
 * load. One source of truth either way — these are the exact files the
 * firmware will embed, not a copy.
 */

import catalogue from "../../profiles/tuya-lock-catalogue.json";
import ds013 from "../../profiles/products/tuya-ds013-t3.json";
import videolock from "../../profiles/products/tuya-t3-videolock.json";
import legacy from "../../profiles/products/ozkie-legacy-v0.json";

import {
  resolveProfile,
  type Catalogue,
  type ProductProfile,
  type ResolvedProfile,
} from "./profile";

/**
 * `$comment` keys are documentation and must not reach the runtime model —
 * `in_lock` picked one up and `Object.keys()` then reported a bond DPID that
 * does not exist. `profileLoad.ts` strips them on read; static imports skip
 * that path, so it has to happen here too.
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

const CAT = stripComments(catalogue) as unknown as Catalogue;

function build(p: unknown): ResolvedProfile {
  const product = stripComments(p) as unknown as ProductProfile;
  return resolveProfile(product, product.standalone ? undefined : CAT);
}

/**
 * Selectable profiles, in the order they should appear in the UI.
 *
 * `ozkie-legacy-v0` is FIRST and is the default — it is what `doorlock-1.58`
 * ships and what the current BANOI build builds frames against, so it is the
 * profile under which the bench currently tells the truth. The real catalogue
 * sits next to it so the same bytes can be reinterpreted without a rebuild:
 * that side-by-side is the clearest demonstration that our DP numbers were
 * invented (`ozkey-27 §2.1`).
 */
export const PROFILES: ResolvedProfile[] = [build(legacy), build(ds013), build(videolock)];

export const PROFILE_IDS = PROFILES.map((p) => p.profile_id);

export function getProfile(id: string): ResolvedProfile {
  return PROFILES.find((p) => p.profile_id === id) ?? PROFILES[0];
}

export const DEFAULT_PROFILE_ID = PROFILES[0].profile_id;
