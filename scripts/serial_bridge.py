"""Bench bridge: forward the ESP32's telemetry JSON from USB serial to the local server.

The firmware prints every telemetry packet on the serial port. Until Wi-Fi/4G is set up,
this script posts those real sensor readings to the server so the website shows live data.
Each packet is tagged VIA_USB_SERIAL. Server replies (anti-theft settings) are NOT sent
back to the device, so arming from the website is not acknowledged in this mode.

Run with the PlatformIO Python (it has pyserial):
  & 'C:\\Users\\yiinn\\.platformio\\penv\\Scripts\\python.exe' scripts/serial_bridge.py
"""
import argparse
import json
import sys
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[1]


def read_env(path):
    values = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def find_esp32():
    for info in list_ports.comports():
        if info.vid == 0x1A86 and info.pid == 0x7523:  # CH340 on the ESP32 board
            return info.device
    return None


def open_port(name):
    device = find_esp32() if name == "auto" else name
    if not device:
        return None
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = device, 115200, 1
    port.dtr = port.rts = False  # opening the port must not reset or reboot the ESP32 into download mode
    try:
        port.open()
    except (serial.SerialException, OSError):
        return None
    print(f"Connected to ESP32 on {device}")
    return port


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="auto", help="COM port, or 'auto' to find the CH340 (survives re-plugging).")
    parser.add_argument("--url", default="http://127.0.0.1:8000")
    parser.add_argument("--env", default=str(ROOT / "server" / ".env"))
    parser.add_argument("--seconds", type=float, default=0, help="Stop after N seconds (0 = run until Ctrl+C).")
    parser.add_argument("--targets", default="local,supabase",
                        help="Where to forward: local (laptop server), supabase, or both (default; Supabase skipped if not configured).")
    args = parser.parse_args()
    env = read_env(args.env)
    token = env.get("DEVICE_TOKEN", "")
    if len(token) < 24:
        sys.exit("DEVICE_TOKEN missing in server/.env")
    supabase_url, supabase_key = env.get("SUPABASE_URL", "").rstrip("/"), env.get("SUPABASE_ANON_KEY", "")
    targets = [t.strip() for t in args.targets.split(",") if t.strip()]
    if "supabase" in targets and not (supabase_url and supabase_key):
        print("Supabase not configured in server/.env; forwarding to the local server only")
        targets = [t for t in targets if t != "supabase"]

    print(f"Forwarding telemetry to: {', '.join(targets)} (Ctrl+C to stop)")
    sent = rejected = 0
    started = time.time()
    port = None
    try:
        while not args.seconds or time.time() - started < args.seconds:
            if port is None:
                port = open_port(args.port)
                if port is None:
                    time.sleep(2)
                    continue
            try:
                line = port.readline().decode("utf-8", errors="replace").strip()
            except (serial.SerialException, OSError):
                print("ESP32 disconnected; waiting for it to come back...")
                port.close()
                port = None
                continue
            if not (line.startswith("{") and '"device_id"' in line):
                continue
            try:
                packet = json.loads(line)
            except json.JSONDecodeError:
                continue
            packet.setdefault("errors", []).append("VIA_USB_SERIAL")
            summary = (f"seq={packet['seq']} temp={packet.get('temperature_c')} accel={packet.get('acceleration_g')} "
                       f"tilt={packet.get('tilt_deg')} motion={packet.get('motion')} gps={packet.get('gps_valid')}")
            outcome = []
            for target in targets:
                if target == "supabase":
                    request = Request(f"{supabase_url}/rest/v1/rpc/ingest_telemetry",
                                      data=json.dumps({"p_device_id": packet.get("device_id"), "p_token": token,
                                                       "p_payload": packet}).encode(),
                                      headers={"Content-Type": "application/json", "apikey": supabase_key,
                                               "Authorization": "Bearer " + supabase_key})
                else:
                    request = Request(args.url.rstrip("/") + "/api/v1/telemetry", data=json.dumps(packet).encode(),
                                      headers={"Content-Type": "application/json", "Authorization": "Bearer " + token})
                try:
                    with urlopen(request, timeout=15) as response:
                        reply = json.load(response)
                    sent += 1
                    outcome.append(f"{target}=ok" + (" (dup)" if reply.get("duplicate") else ""))
                except HTTPError as error:
                    rejected += 1
                    outcome.append(f"{target}=HTTP {error.code} {error.read()[:120]!r}")
                except (URLError, OSError) as error:
                    outcome.append(f"{target}=unreachable ({getattr(error, 'reason', error)})")
            print(summary, "->", ", ".join(outcome))
    except KeyboardInterrupt:
        pass
    finally:
        if port is not None:
            port.close()
    print(f"sent={sent} rejected={rejected}")


if __name__ == "__main__":
    main()
