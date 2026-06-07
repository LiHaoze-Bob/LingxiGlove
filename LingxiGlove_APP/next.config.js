/** @type {import('next').NextConfig} */
const nextConfig = {
  // Enable WebGL for Three.js
  webpack: (config) => {
    config.resolve.alias = {
      ...config.resolve.alias,
      three$: require.resolve('three'),
    };
    return config;
  },
};

module.exports = nextConfig;
