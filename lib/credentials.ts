/**
 * LocalStorage-backed credential database for time-restricted entry.
 * Mirrors the slot table a real lock MCU keeps in its EEPROM/flash.
 */

export type CredentialKind = "PIN" | "RFID";

export interface StoredCredential {
  kind: CredentialKind;
  slot: number;
  /** PIN digits ("482915") or RFID UID hex ("04 A3 7F 1C"). */
  value: string;
  /** Validity window, unix seconds. */
  start: number;
  end: number;
}

const STORAGE_KEY = "locksim.credentials.v1";

export function loadCredentials(): StoredCredential[] {
  if (typeof window === "undefined") return [];
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    return raw ? (JSON.parse(raw) as StoredCredential[]) : [];
  } catch {
    return [];
  }
}

export function saveCredentials(creds: StoredCredential[]): void {
  if (typeof window === "undefined") return;
  window.localStorage.setItem(STORAGE_KEY, JSON.stringify(creds));
}

/** Insert or replace the credential occupying (kind, slot). */
export function upsertCredential(
  creds: StoredCredential[],
  next: StoredCredential
): StoredCredential[] {
  const rest = creds.filter((c) => !(c.kind === next.kind && c.slot === next.slot));
  return [...rest, next].sort((a, b) => a.kind.localeCompare(b.kind) || a.slot - b.slot);
}

export function deleteCredential(
  creds: StoredCredential[],
  kind: CredentialKind,
  slot: number
): StoredCredential[] {
  return creds.filter((c) => !(c.kind === kind && c.slot === slot));
}

export type TemporalCheck = "VALID" | "NOT_YET_ACTIVE" | "EXPIRED";

/** Strict temporal check of a credential window against the Virtual Master Clock. */
export function checkWindow(cred: StoredCredential, virtualNowMs: number): TemporalCheck {
  const nowSec = Math.floor(virtualNowMs / 1000);
  if (nowSec < cred.start) return "NOT_YET_ACTIVE";
  if (nowSec > cred.end) return "EXPIRED";
  return "VALID";
}
