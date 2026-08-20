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
import wifipro from "../../profiles/products/tuya-wifi-lock-pro.json";
import luona from "../../profiles/products/tuya-luona-ds013-t3.json";
import ladin from "../../profiles/products/tuya-ladin-f7-t3.json";

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
 *
 * `tuya-generic-lock` is the one to reach for when asking "what would a maker
 * we have no datasheet for actually do to us?" — the standard pre-flashed
 * Tuya DL-MCU, 34 DPs, 9 of them `reserved` and refusing loudly. See
 * `docs/DPSuppliers/genericDPList.md`.
 *
 * 🔴 `ozsim-fullfeature` IS DELIBERATELY NOT LISTED (removed 2026-08-20, at the
 * operator's request after it cost a bench session).
 *
 * It was added on 2026-08-18 so the UI could select the profile whose PID
 * LockSim used to answer `0x01` with. That reason is gone: a profile with no
 * PID now stays SILENT rather than falling back to `OZSIM_PID`
 * (see useLockState.ts), so nothing reaches this profile by accident any more.
 *
 * Why it must not be selectable: it is a FICTION that carries no credential DP
 * of any kind, so a lock running it accepts a PIN grant and silently drops it —
 * both ends reporting success. That is exactly what happened on the bench on
 * 2026-08-20 (`ozkey-42 §2.4`). Every honest thing it could be used for
 * (exercising the real access-event DPs) `tuya-luona-ds013-t3` does better, with a
 * real PID and a real supplier document behind it.
 *
 * The JSON stays in `profiles/products/` — firmware still globs it into
 * `ozprofile_gen.h` and it remains a valid target there. This removes the
 * FOOT-GUN in the operator's dropdown, not the profile.
 */
export const PROFILES: ResolvedProfile[] = [
  build(wifipro),
  build(luona),
  build(ladin),
];

export const PROFILE_IDS = PROFILES.map((p) => p.profile_id);

export function getProfile(id: string): ResolvedProfile {
  return PROFILES.find((p) => p.profile_id === id) ?? PROFILES[0];
}

export const DEFAULT_PROFILE_ID = PROFILES[0].profile_id;
