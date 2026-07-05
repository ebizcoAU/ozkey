"use client";

import { ONBOARDING_TOPIC, buildSampleOnboarding } from "@/lib/provisioning";

interface OnboardingPanelProps {
  value: string;
  onChange: (raw: string) => void;
  onPublish: (raw: string) => void;
  deviceMac: string;
  className?: string;
}

/**
 * OZKEYSERV/ onboarding handshake editor. Publishes a JSON payload onto the
 * `hotel/rooms/+/lock/command` MQTT topic so the provisioning parser can pair
 * the lock. State is lifted to the page so it survives being relocated between
 * the console column and its own dedicated column at wide viewports.
 */
export default function OnboardingPanel({
  value,
  onChange,
  onPublish,
  deviceMac,
  className = "",
}: OnboardingPanelProps) {
  const publish = () => {
    if (value.trim()) onPublish(value);
  };

  return (
    <div className={`rounded-lg border border-blue-900/50 bg-neutral-900/70 p-3 ${className}`}>
      <div className="mb-1.5 flex items-center justify-between">
        <span className="text-[10px] font-bold uppercase tracking-widest text-blue-400">
          ⛁ OZKEYSERV/ Onboarding Handshake
        </span>
        <span className="font-mono text-[9px] text-neutral-500">{ONBOARDING_TOPIC}</span>
      </div>
      <textarea
        value={value}
        onChange={(e) => onChange(e.target.value)}
        spellCheck={false}
        rows={9}
        className="w-full resize-y rounded border border-neutral-700 bg-black px-3 py-2 font-mono text-[11px] leading-relaxed text-blue-300 outline-none focus:border-blue-700"
      />
      <div className="mt-2 flex flex-wrap items-center gap-2">
        <button
          type="button"
          onClick={publish}
          className="rounded border border-blue-700 bg-blue-900/40 px-4 py-1.5 text-[11px] font-bold uppercase tracking-wider text-blue-200 hover:bg-blue-800/40"
        >
          Publish to Lock
        </button>
        <button
          type="button"
          onClick={() => onChange(buildSampleOnboarding(deviceMac, "412"))}
          className="rounded border border-neutral-700 bg-neutral-800 px-3 py-1.5 text-[10px] text-neutral-300 hover:bg-neutral-700"
        >
          Load valid handshake (Room 412)
        </button>
        <button
          type="button"
          onClick={() => onChange(buildSampleOnboarding("00:11:22:33:44:55", "999"))}
          className="rounded border border-neutral-700 bg-neutral-800 px-3 py-1.5 text-[10px] text-neutral-300 hover:bg-neutral-700"
        >
          Load MAC mismatch (rejected)
        </button>
        <span className="text-[9px] text-neutral-600">Requires BLE mode + a matching MAC to pair</span>
      </div>
    </div>
  );
}
