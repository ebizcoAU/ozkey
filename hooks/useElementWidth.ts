"use client";

import { useEffect, useRef, useState } from "react";

/**
 * Track an element's rendered width with a ResizeObserver. Used to decide the
 * page layout dynamically (e.g. promote the onboarding panel to its own column
 * only when the measured width can actually accommodate a third column).
 */
export function useElementWidth<T extends HTMLElement>() {
  const ref = useRef<T>(null);
  const [width, setWidth] = useState(0);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const observer = new ResizeObserver((entries) => {
      const entry = entries[0];
      if (entry) setWidth(entry.contentRect.width);
    });
    observer.observe(el);
    setWidth(el.getBoundingClientRect().width);
    return () => observer.disconnect();
  }, []);

  return { ref, width };
}
