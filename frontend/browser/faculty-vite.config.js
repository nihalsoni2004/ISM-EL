import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import projectConfig from '../vite.config.js';

export default async (context) => {
  const config = projectConfig(context);
  const session = process.env.FINAL_DEMO_ROOT;
  const run = process.env.FINAL_DEMO_RUN;
  if (!session || !/^[0-9a-f]{32}$/.test(run || '')) throw new Error('Expected a tracked faculty demo');
  const identity = JSON.parse(await readFile(resolve(session, 'demo.json'), 'utf8'));
  const marker = resolve(session, 'frontend-' + run + '.stop');
  return {
    ...config,
    plugins: [...config.plugins, {
      name: 'faculty-demo-graceful-stop',
      configureServer(server) {
        const timer = setInterval(async () => {
          let request;
          try { request = JSON.parse(await readFile(marker, 'utf8')); }
          catch { return; }
          if (request.run !== run || request.demo_token !== identity.token) return;
          clearInterval(timer);
          await server.close();
        }, 200);
        timer.unref();
        server.httpServer?.once('close', () => clearInterval(timer));
      },
    }],
  };
};
