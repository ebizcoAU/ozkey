"use client";

import { blockedEntries, usableEntries, type ResolvedProfile } from "@/lib/profile";
import { PROFILES } from "@/lib/profileRegistry";

interface ProfileSelectorProps {
  profile: ResolvedProfile;
  onChange: (id: string) => void;
}

/**
 * Which supplier's DP map the console is interpreting under.
 *
 * This control exists to make one specific thing impossible to miss. Until
 * 2026-08-13 LockSim had a single hardcoded DP map, every number in it was
 * INVENTED, and the console asserted that interpretation as fact — so the bench
 * could only ever confirm that we agreed with ourselves (`ozkey-27 §2.1`).
 *
 * Switching between `ozkie-legacy-v0` and `tuya-ds013-t3` re-labels the same
 * bytes: a DP 21 write reads "Add Temporary PIN" under our fiction and
 * `navigation_volume` under the real Tuya catalogue. Seeing one frame change
 * meaning is the shortest path to understanding why the map had to become data.
 */
export default function ProfileSelector({ profile, onChange }: ProfileSelectorProps) {
  const usable = usableEntries(profile).length;
  const blocked = blockedEntries(profile).length;
  const isFiction = profile.deprecated;

  return (
    <div className="flex flex-col gap-1 rounded border border-neutral-800 bg-neutral-900/60 px-2 py-1.5">
      <div className="flex items-center gap-2">
        <span className="text-[10px] font-semibold tracking-wide text-neutral-500">
          DP PROFILE
        </span>
        <select
          value={profile.profile_id}
          onChange={(e) => onChange(e.target.value)}
          className="min-w-0 flex-1 rounded border border-neutral-700 bg-neutral-950 px-1.5 py-0.5 text-[11px] text-neutral-200 outline-none focus:border-sky-600"
        >
          {PROFILES.map((p) => (
            <option key={p.profile_id} value={p.profile_id}>
              {p.profile_id}
              {p.deprecated ? " (fiction)" : ""}
            </option>
          ))}
        </select>
      </div>

      <div className="flex items-center gap-2 text-[10px]">
        {isFiction ? (
          // Not a soft warning. This profile is the reason ozkey-21 §8.3 was
          // unanswerable at the bench for weeks.
          <span className="rounded bg-red-950 px-1 text-red-300">
            ⚠ INVENTED MAP — collides with every Tuya lock supplier
          </span>
        ) : (
          <>
            <span className="text-green-400">{usable} usable</span>
            <span className="text-neutral-600">·</span>
            {/* `reserved` is not a bug: the DP and its type are documented and
                the payload layout is not, so we refuse rather than guess. */}
            <span className="text-amber-400">{blocked} blocked</span>
            <span className="text-neutral-600">·</span>
            <span className="text-neutral-500">awaiting supplier layouts</span>
          </>
        )}
      </div>
    </div>
  );
}
