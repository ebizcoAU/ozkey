import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // We keep our own lockfile; pin the tracing root so Next doesn't pick the
  // parent-directory lockfile it detected.
  outputFileTracingRoot: __dirname,
};

export default nextConfig;
