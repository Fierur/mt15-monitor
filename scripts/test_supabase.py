"""Self-test the Supabase ingest function from the laptop (no ESP32 needed).

Needs two lines in server/.env (the publishable/anon key is public; never use the service_role key):
  SUPABASE_URL=https://bfrzafaaduvrnbppjdbr.supabase.co
  SUPABASE_ANON_KEY=<publishable or anon key>

Run:  .\\.venv\\Scripts\\python.exe scripts\\test_supabase.py
Test rows use boot_id "selftest-..." and the SELFTEST error tag; remove them afterwards with:
  delete from public.alerts where telemetry_id in (select id from public.telemetry where boot_id like 'selftest-%');
  delete from public.device_latest where boot_id like 'selftest-%';
  delete from public.telemetry where boot_id like 'selftest-%';
"""
import json
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
env = {}
for line in (ROOT / "server" / ".env").read_text(encoding="utf-8").splitlines():
    if "=" in line and not line.lstrip().startswith("#"):
        key, value = line.split("=", 1)
        env[key.strip()] = value.strip()
url, anon = env.get("SUPABASE_URL", "").rstrip("/"), env.get("SUPABASE_ANON_KEY", "")
device, token = env.get("DEVICE_ID", "mt15-001"), env.get("DEVICE_TOKEN", "")
if not url or not anon:
    raise SystemExit("Add SUPABASE_URL and SUPABASE_ANON_KEY to server/.env first.")


def ingest(payload, device_token=token):
    body = json.dumps({"p_device_id": device, "p_token": device_token, "p_payload": payload}).encode()
    request = Request(f"{url}/rest/v1/rpc/ingest_telemetry", data=body, method="POST",
                      headers={"apikey": anon, "Authorization": f"Bearer {anon}", "Content-Type": "application/json"})
    try:
        with urlopen(request, timeout=20) as response:
            return response.status, json.load(response)
    except HTTPError as error:
        return error.code, error.read().decode(errors="replace")[:200]


boot = "selftest-" + uuid.uuid4().hex[:10]
base = dict(device_id=device, boot_id=boot, uptime_s=5, mode="parked", armed=False, config_revision=0,
            temperature_c=30.5, gps_valid=True, latitude=10.776900, longitude=106.700900, speed_kmh=0.0,
            satellites=8, acceleration_g=0.98, gyro_dps=0.1, motion=False, tilt_deg=2.0, fallen=False,
            next_report_s=5, errors=["SELFTEST"], measured_at=datetime.now(timezone.utc).isoformat())
results = []


def check(name, ok, detail):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")


status, reply = ingest({**base, "seq": 1})
check("valid packet accepted", status == 200 and isinstance(reply, dict) and reply.get("accepted") and not reply.get("duplicate"), (status, reply))
if isinstance(reply, dict):
    check("reply shape the firmware expects", reply.get("ack") == {"boot_id": boot, "seq": 1}
          and set(reply.get("settings", {})) == {"armed", "revision"}, reply)
status, reply = ingest({**base, "seq": 1})
check("same seq is a duplicate", status == 200 and isinstance(reply, dict) and reply.get("duplicate"), (status, reply))
status, reply = ingest({**base, "seq": 2}, device_token="wrong-token-0123456789-abcdef")
check("wrong token rejected (403, device keeps data)", status in (401, 403), (status, reply))
status, reply = ingest({**base, "seq": 3, "gps_valid": True, "latitude": None})
check("invalid payload rejected (400, device drops it)", status == 400, (status, reply))
status, reply = ingest({**base, "seq": 4, "tilt_deg": 85.0, "fallen": True})
check("fall packet accepted (creates a 'fall' alert)", status == 200, (status, reply))
print(f"\n{sum(results)}/{len(results)} checks passed. Boot id of the test rows: {boot}")
