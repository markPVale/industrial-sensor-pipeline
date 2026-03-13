import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // Required to allow mqtt.js (a Node.js library) to be bundled
  // for browser use. Next.js will polyfill the missing Node modules.
  webpack: (config, { isServer }) => {
    if (!isServer) {
      config.resolve.fallback = {
        ...config.resolve.fallback,
        net: false,
        tls: false,
        fs: false,
      };
    }
    return config;
  },
};

export default nextConfig;
