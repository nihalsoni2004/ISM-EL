import { defineConfig, loadEnv } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig(({ mode }) => {
  const { VITE_PROXY_TARGET } = loadEnv(mode, process.cwd());
  const target = VITE_PROXY_TARGET || 'http://127.0.0.1:8000';
  return {
    plugins: [react()],
    server: {
      host: '127.0.0.1',
      port: 5173,
      strictPort: true,
      proxy: { '/health': { target, changeOrigin: true }, '/api': { target, changeOrigin: true } },
    },
    test: { environment: 'jsdom', setupFiles: './src/tests/setup.js', include: ['src/tests/**/*.test.{js,jsx}'] },
  };
});
