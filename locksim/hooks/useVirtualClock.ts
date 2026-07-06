"use client";

import { useCallback, useEffect, useRef, useState } from "react";

/**
 * Virtual Master Clock: real time plus a developer-controlled warp offset.
 * All temporal credential checks read this clock, never Date.now() directly.
 */
export function useVirtualClock() {
  const [offsetMs, setOffsetMs] = useState(0);
  const [virtualNowMs, setVirtualNowMs] = useState(() => Date.now());
  const offsetRef = useRef(0);
  offsetRef.current = offsetMs;

  useEffect(() => {
    const tick = () => setVirtualNowMs(Date.now() + offsetRef.current);
    tick();
    const iv = setInterval(tick, 250);
    return () => clearInterval(iv);
  }, []);

  /** Stable getter for use inside timers/callbacks without stale closures. */
  const now = useCallback(() => Date.now() + offsetRef.current, []);

  const warpBy = useCallback((deltaMs: number) => {
    setOffsetMs((o) => o + deltaMs);
    setVirtualNowMs(Date.now() + offsetRef.current + deltaMs);
  }, []);

  const warpTo = useCallback((targetMs: number) => {
    const nextOffset = targetMs - Date.now();
    setOffsetMs(nextOffset);
    setVirtualNowMs(targetMs);
  }, []);

  const reset = useCallback(() => {
    setOffsetMs(0);
    setVirtualNowMs(Date.now());
  }, []);

  return { virtualNowMs, offsetMs, now, warpBy, warpTo, reset };
}

export type VirtualClockApi = ReturnType<typeof useVirtualClock>;
