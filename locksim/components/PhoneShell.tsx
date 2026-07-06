"use client";

/** iPhone-styled chassis: rounded aluminum frame, notch, side buttons. */
export default function PhoneShell({ children }: { children: React.ReactNode }) {
  return (
    <div className="relative shrink-0">
      {/* Side hardware buttons */}
      <div className="absolute -left-[3px] top-28 h-8 w-[3px] rounded-l bg-neutral-600" />
      <div className="absolute -left-[3px] top-40 h-14 w-[3px] rounded-l bg-neutral-600" />
      <div className="absolute -left-[3px] top-56 h-14 w-[3px] rounded-l bg-neutral-600" />
      <div className="absolute -right-[3px] top-40 h-20 w-[3px] rounded-r bg-neutral-600" />

      <div className="w-[390px] rounded-[3.2rem] border border-neutral-700 bg-gradient-to-b from-neutral-700 via-neutral-800 to-neutral-700 p-[10px] shadow-[0_0_60px_rgba(0,0,0,0.9),inset_0_0_2px_rgba(255,255,255,0.3)]">
        <div className="relative overflow-hidden rounded-[2.6rem] bg-neutral-900 ring-1 ring-black">
          {/* Speaker notch */}
          <div className="absolute left-1/2 top-0 z-20 flex h-7 w-40 -translate-x-1/2 items-center justify-center gap-2 rounded-b-2xl bg-black">
            <div className="h-1.5 w-12 rounded-full bg-neutral-800" />
            <div className="h-2.5 w-2.5 rounded-full bg-neutral-800 ring-1 ring-neutral-700" />
          </div>
          <div className="pt-9">{children}</div>
          {/* Home indicator */}
          <div className="flex justify-center pb-2 pt-1">
            <div className="h-1 w-32 rounded-full bg-neutral-600" />
          </div>
        </div>
      </div>
    </div>
  );
}
