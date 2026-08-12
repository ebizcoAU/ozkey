/**
 * Device profiles — the DP map as DATA, shared with the doorlock firmware.
 *
 * WHY THIS EXISTS
 * ---------------
 * Until 2026-08-12 LockSim carried a hand-written `DpId` enum in `tuya.ts`.
 * Every number in it was INVENTED, and every one collides with the standard
 * Tuya lock catalogue (`ozkey-27 §2.1`): our "add temp PIN" DP 21 is really
 * `navigation_volume`; DP 23 is `auto_lock`; DP 24 is `auto_lock_delay`; and
 * DP 60 — which our firmware treats as a pairing gesture — is the 18-value
 * alarm channel.
 *
 * The consequence was worse than a wrong constant: LockSim and the doorlock
 * firmware each held their OWN copy of the fiction, so the bench could only
 * ever confirm that we agreed with ourselves. That is why `ozkey-21 §8.3`
 * ("does the MCU self-expire credentials?") was unanswerable here.
 *
 * The fix is not a corrected enum. It is removing the ability of the two ends
 * of the wire to disagree: `profiles/` holds ONE JSON per product, and both
 * LockSim and the firmware resolve against it.
 *
 * CATALOGUE + SELECTION
 * ---------------------
 * DP numbers are not per-manufacturer. They come from a standard Tuya
 * catalogue per product category, and a manufacturer selects a subset at PID
 * creation — proven by DP 42 and DP 76 carrying identical meaning across two
 * different products in two different categories (`ozkey-27 §3`). So a dozen
 * suppliers is one catalogue plus a dozen short selection files, not a dozen
 * DP lists.
 *
 * A profile may instead set `standalone: true` and carry its own entries. Only
 * `ozkie-legacy-v0` does, because it contradicts the catalogue rather than
 * subsetting it.
 */

export type DpDirection = "up" | "down" | "both";

/**
 * How much we actually know about an entry. `reserved` is the load-bearing
 * one: DS013-T3 documents the *type* of every credential-write DP and the
 * payload layout of none of them (`ozkey-27 §2.5`), so those must fail loudly
 * rather than send plausible-looking bytes at a door lock.
 */
export type DpStatus =
  /** Type AND payload semantics documented by the supplier. */
  | "confirmed"
  /** DP and type known, payload layout NOT supplied — reject with UNSUPPORTED. */
  | "reserved"
  /** Seen but unestablished — log id + length only, never publish the payload. */
  | "unknown"
  /** We made this up. Only ever in `ozkie-legacy-v0`. */
  | "fiction";

export type DpValueType = "raw" | "bool" | "value" | "string" | "enum" | "bitmap";

export interface DpEntry {
  dp: number;
  /** Tuya's catalogue name, verbatim. Renaming a published standard is how we got here. */
  name: string;
  type: DpValueType;
  dir: DpDirection;
  status: DpStatus;
  /** OZKIE v1 verb this maps to (`ozkey-28`). */
  verb?: string;
  /** Which arg/field of that verb the value lands in. */
  field?: string;
  /** For `event.access`: which credential class produced it. */
  access_kind?: string;
  /** ENUM only: wire value (as a decimal string) -> stable name. */
  enum?: Record<string, string>;
  /** VALUE only: inclusive [min, max]. */
  range?: [number, number];
  unit?: string;
  max_len?: number;
  /** RAW/STRING only: the codec needed to encode/decode. Absent => cannot be built. */
  codec?: string;
  /** Populated on `reserved` — what we are waiting for. */
  blocked_by?: string;
  /** Populated on `fiction` — what the number really means. */
  collides_with?: string;
}

export interface Catalogue {
  catalogue_id: string;
  rev: number;
  sources: string[];
  entries: DpEntry[];
}

export interface ProductProfile {
  profile_id: string;
  rev: number;
  catalogue?: string;
  standalone?: boolean;
  deprecated?: boolean;
  complete?: boolean;
  supplier?: Record<string, string>;
  source?: Record<string, string>;
  cmd?: Record<string, unknown>;
  link?: Record<string, unknown>;
  features?: Record<string, unknown>;
  selects?: number[];
  overrides?: DpEntry[];
  extra?: DpEntry[];
  entries?: DpEntry[];
  in_lock?: Record<string, string>;
}

/** A profile with its catalogue merged in and indexed for lookup. */
export interface ResolvedProfile {
  profile_id: string;
  rev: number;
  deprecated: boolean;
  /** Every entry this product carries, ordered by DP id. */
  entries: DpEntry[];
  cmd?: Record<string, unknown>;
  link?: Record<string, unknown>;
  features?: Record<string, unknown>;
  in_lock: Record<string, string>;
  byDp: Map<number, DpEntry>;
  /** verb -> entries. One verb can be served by several DPs (e.g. event.access). */
  byVerb: Map<string, DpEntry[]>;
}

export class ProfileError extends Error {}

/**
 * Merge a product profile with its catalogue.
 *
 * Order is `selects` -> `overrides` -> `extra`, so a product can narrow an
 * enum or range the catalogue states more broadly without forking the entry.
 */
export function resolveProfile(product: ProductProfile, catalogue?: Catalogue): ResolvedProfile {
  const byDp = new Map<number, DpEntry>();

  if (product.standalone) {
    if (!product.entries?.length) {
      throw new ProfileError(`${product.profile_id}: standalone profile has no entries`);
    }
    for (const e of product.entries) byDp.set(e.dp, e);
  } else {
    if (!catalogue) {
      throw new ProfileError(
        `${product.profile_id}: needs catalogue '${product.catalogue}' but none was supplied`
      );
    }
    const cat = new Map(catalogue.entries.map((e) => [e.dp, e]));
    for (const dp of product.selects ?? []) {
      const entry = cat.get(dp);
      if (!entry) {
        throw new ProfileError(
          `${product.profile_id}: selects DP ${dp}, absent from catalogue '${catalogue.catalogue_id}'`
        );
      }
      byDp.set(dp, entry);
    }
    // Overrides must narrow something that was selected — an override for an
    // unselected DP is a typo, not a feature, and silently adding it would
    // hide the mistake.
    for (const o of product.overrides ?? []) {
      const base = byDp.get(o.dp);
      if (!base) {
        throw new ProfileError(
          `${product.profile_id}: overrides DP ${o.dp}, which it does not select`
        );
      }
      byDp.set(o.dp, { ...base, ...o });
    }
    for (const x of product.extra ?? []) {
      if (byDp.has(x.dp)) {
        throw new ProfileError(
          `${product.profile_id}: extra DP ${x.dp} duplicates a selected entry — use overrides`
        );
      }
      byDp.set(x.dp, x);
    }
  }

  const entries = [...byDp.values()].sort((a, b) => a.dp - b.dp);

  const byVerb = new Map<string, DpEntry[]>();
  for (const e of entries) {
    if (!e.verb) continue;
    const list = byVerb.get(e.verb) ?? [];
    list.push(e);
    byVerb.set(e.verb, list);
  }

  return {
    profile_id: product.profile_id,
    rev: product.rev,
    deprecated: product.deprecated === true,
    entries,
    cmd: product.cmd,
    link: product.link,
    features: product.features,
    in_lock: product.in_lock ?? {},
    byDp,
    byVerb,
  };
}

/** What the firmware/emulator should do with a DP that just arrived. */
export type DpDisposition =
  | { action: "handle"; entry: DpEntry }
  /** Known DP, payload layout not supplied — answer UNSUPPORTED, send nothing. */
  | { action: "unsupported"; entry: DpEntry; reason: string }
  /** Not in the profile at all — log id + length, never the payload. */
  | { action: "reject"; reason: string };

/**
 * The dispatch gate, replacing `ozDpForwardable()`'s hardcoded
 * `dp == 1 || (dp >= 21 && dp <= 24)`.
 *
 * That allow-list was built around the invented map, so against a real
 * DS013-T3 it permits exactly the SETTINGS DPs (volume, auto-lock, delay) and
 * blocks every credential operation — the precise inverse of its intent.
 */
export function dispositionFor(
  profile: ResolvedProfile,
  dp: number,
  direction: DpDirection
): DpDisposition {
  const entry = profile.byDp.get(dp);
  if (!entry) return { action: "reject", reason: `DP ${dp} not in profile ${profile.profile_id}` };

  if (entry.dir !== "both" && entry.dir !== direction) {
    return {
      action: "reject",
      reason: `DP ${dp} (${entry.name}) is ${entry.dir}-only, got ${direction}`,
    };
  }

  if (entry.status === "reserved" || entry.status === "unknown") {
    return {
      action: "unsupported",
      entry,
      reason: entry.blocked_by ?? `DP ${dp} (${entry.name}) is ${entry.status}`,
    };
  }

  // A RAW/STRING entry with no codec cannot be built or parsed, whatever its
  // status claims. Catching it here means a profile authoring slip surfaces as
  // a refusal rather than as malformed bytes on the wire.
  if ((entry.type === "raw" || entry.type === "string") && !entry.codec) {
    return {
      action: "unsupported",
      entry,
      reason: `DP ${dp} (${entry.name}) is ${entry.type} with no codec`,
    };
  }

  return { action: "handle", entry };
}

/** Decode an ENUM wire value to its stable name (`14` -> `"pry"`). */
export function enumName(entry: DpEntry, value: number): string | undefined {
  return entry.enum?.[String(value)];
}

/** Encode a stable enum name back to its wire value (`"pry"` -> `14`). */
export function enumValue(entry: DpEntry, name: string): number | undefined {
  if (!entry.enum) return undefined;
  for (const [k, v] of Object.entries(entry.enum)) if (v === name) return Number(k);
  return undefined;
}

/** Entries a caller can actually use today — for UI, and for "what is blocked". */
export function usableEntries(profile: ResolvedProfile): DpEntry[] {
  return profile.entries.filter((e) => e.status === "confirmed" || e.status === "fiction");
}

export function blockedEntries(profile: ResolvedProfile): DpEntry[] {
  return profile.entries.filter((e) => e.status === "reserved" || e.status === "unknown");
}
