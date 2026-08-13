import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // We keep our own lockfile; pin the tracing root so Next doesn't pick the
  // parent-directory lockfile it detected.
  outputFileTracingRoot: __dirname,

  // 🔴 A verification build MUST NOT share `.next` with a running dev server.
  //
  // `next build` and `next dev` both write `.next`, and a production build
  // overwrites the dev server's chunks and client manifest underneath it. The
  // live server then fails with "Cannot find module './833.js'" and
  // "Could not find the module ... in the React Client Manifest" — which reads
  // like application breakage and is not; it is one process pulling the floor
  // out from under another. Cost the operator a working bench while I was
  // running build-checks alongside his `npm run dev`.
  //
  // `npm run build:check` sets NEXT_BUILD_CHECK and lands in `.next-check`, so
  // typecheck/compile verification can run at any time without touching a live
  // dev server. `npm run build` is unchanged for real production output.
  distDir: process.env.NEXT_BUILD_CHECK ? ".next-check" : ".next",
};

export default nextConfig;
