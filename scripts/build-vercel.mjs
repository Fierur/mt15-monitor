import { copyFileSync, cpSync, mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const url = process.env.SUPABASE_URL?.trim().replace(/\/+$/, '');
const key = process.env.SUPABASE_PUBLISHABLE_KEY?.trim();
const deviceId = process.env.DEVICE_ID?.trim();

if (url !== 'https://bfrzafaaduvrnbppjdbr.supabase.co') {
  throw new Error('SUPABASE_URL must match the MT15 project allowed by the Vercel content security policy.');
}
if (!/^sb_publishable_[A-Za-z0-9_-]+$/.test(key ?? '')) {
  throw new Error('SUPABASE_PUBLISHABLE_KEY must be a Supabase publishable key.');
}
if (!/^[A-Za-z0-9][A-Za-z0-9_-]*$/.test(deviceId ?? '')) {
  throw new Error('DEVICE_ID must contain letters, numbers, underscores or hyphens.');
}

const source = join(root, 'server', 'static');
const output = join(root, 'dist');
mkdirSync(output, { recursive: true });
cpSync(source, join(output, 'static'), { recursive: true });
copyFileSync(join(source, 'index.html'), join(output, 'index.html'));
writeFileSync(join(output, 'dashboard-config.json'), JSON.stringify({
  mode: 'supabase',
  url,
  key,
  device_id: deviceId,
}) + '\n');
