"""Write supabase/device.sql: registers the device using the DEVICE_ID/DEVICE_TOKEN already in server/.env.

Only the SHA-256 hash of the token goes into the SQL file, so the file is safe to paste into the
Supabase SQL Editor. The firmware keeps sending the plain token, exactly as it does today.
"""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
env = {}
for line in (ROOT / "server" / ".env").read_text(encoding="utf-8").splitlines():
    if "=" in line and not line.lstrip().startswith("#"):
        key, value = line.split("=", 1)
        env[key.strip()] = value.strip()
device_id, token = env.get("DEVICE_ID", "mt15-001"), env.get("DEVICE_TOKEN", "")
if len(token) < 24:
    raise SystemExit("DEVICE_TOKEN missing or too short in server/.env")
digest = hashlib.sha256(token.encode()).hexdigest()
sql = f"""-- Register the MT15 device (run AFTER schema.sql). Contains only the token hash, not the token.
insert into public.devices (id, name, token_hash)
values ('{device_id}', 'Yamaha MT15', '{digest}')
on conflict (id) do update set token_hash = excluded.token_hash;

insert into public.settings (device_id) values ('{device_id}') on conflict (device_id) do nothing;

-- Make your login an admin: first create the user in Dashboard -> Authentication -> Users -> Add user,
-- then replace the e-mail below and run these two lines.
-- insert into public.admins (user_id)
-- select id from auth.users where email = 'ban@example.com' on conflict do nothing;
"""
out = ROOT / "supabase" / "device.sql"
out.write_text(sql, encoding="utf-8")
print(f"Wrote {out} for device {device_id}")
