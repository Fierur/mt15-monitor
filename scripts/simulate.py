"""Generate labelled bench telemetry for an isolated demo server (never impersonate live hardware)."""
import argparse
import json
import math
import os
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.request import Request, urlopen

from dotenv import load_dotenv

p = argparse.ArgumentParser()
p.add_argument('--url', default='http://127.0.0.1:8001', help='Demo server URL (not the real server on 8000).')
p.add_argument('--count', type=int, default=12)
p.add_argument('--interval', type=float, default=5)
p.add_argument('--motion', action='store_true', help='Send movement; may trigger configured real email when armed.')
p.add_argument('--env', required=True, help='Explicit env file of a separate demo server, e.g. server/.env.demo.')
a = p.parse_args()
if Path(a.env).resolve() == (Path(__file__).resolve().parents[1] / 'server' / '.env'):
    raise SystemExit('Refusing to simulate with the real server/.env. Use server/.env.demo.')
load_dotenv(Path(a.env), override=True)
boot = 'sim-' + uuid.uuid4().hex[:12]
armed, revision = False, 0
for seq in range(a.count):
    payload = dict(device_id=os.environ['DEVICE_ID'], boot_id=boot, seq=seq,
                   uptime_s=int(seq*a.interval), mode='bench',
                   measured_at=datetime.now(timezone.utc).isoformat(), armed=armed,
                   config_revision=revision, battery_v=12.6+0.15*math.sin(seq/4),
                   temperature_c=30+math.sin(seq/3), gps_valid=True,
                   latitude=10.7769+seq*0.00001, longitude=106.7009+seq*0.00001,
                   speed_kmh=10, satellites=8, motion=a.motion,
                   errors=['SIMULATED_DATA'], next_report_s=max(5,int(a.interval)))
    req = Request(a.url.rstrip('/')+'/api/v1/telemetry', data=json.dumps(payload).encode(),
                  headers={'Content-Type':'application/json','Authorization':'Bearer '+os.environ['DEVICE_TOKEN']})
    with urlopen(req, timeout=20) as r: reply=json.load(r)
    armed, revision=reply['settings']['armed'], reply['settings']['revision']
    print(f"SIMULATED sample {seq}: accepted={reply['accepted']}")
    if seq+1<a.count: time.sleep(a.interval)
