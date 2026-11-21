"""Generate an ignored firmware header, without displaying the device token."""
import argparse
import hashlib
import json
import re
import ssl
from pathlib import Path

import certifi
from dotenv import dotenv_values


ROOT = Path(__file__).resolve().parents[1]


def certificate(label: str) -> str:
    store = Path(certifi.where()).read_text(encoding="ascii")
    match = re.search(r'^# Label: "' + re.escape(label) + r'"[^\n]*\n'
                      r'(?:(?!-----BEGIN)[^\n]*\n)*(-----BEGIN CERTIFICATE-----\n'
                      r'.*?-----END CERTIFICATE-----\n)', store, re.M | re.S)
    if not match:
        raise SystemExit(f"Root CA missing from certifi: {label}")
    return match[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apn", required=True)
    parser.add_argument("--enable-4g", action="store_true",
                        help="Enable after the correct and incorrect TLS host tests pass")
    args = parser.parse_args()
    env = dotenv_values(ROOT / "server" / ".env")
    url = (env.get("SUPABASE_URL") or "").rstrip("/")
    key = env.get("SUPABASE_PUBLISHABLE_KEY") or env.get("SUPABASE_ANON_KEY") or ""
    device = env.get("DEVICE_ID") or "mt15-001"
    token = env.get("DEVICE_TOKEN") or ""
    if not re.fullmatch(r"https://[a-z0-9-]+\.supabase\.co", url):
        raise SystemExit("Invalid local SUPABASE_URL")
    if not re.fullmatch(r"sb_publishable_[A-Za-z0-9_-]+", key):
        raise SystemExit("Supabase publishable key missing or invalid")
    if (not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", device)
        or not re.fullmatch(r"[A-Za-z0-9_-]{24,}", token)
        or not re.fullmatch(r"[A-Za-z0-9.-]{1,100}", args.apn)):
        raise SystemExit("Invalid device ID, token or APN")
    ca = certificate("GTS Root R4")
    fingerprint = hashlib.sha256(ssl.PEM_cert_to_DER_cert(ca)).hexdigest()[:16]
    settings = {
        "SUPABASE_URL": url, "SUPABASE_PUBLISHABLE_KEY": key, "DEVICE_ID": device,
        "DEVICE_TOKEN": token, "CELLULAR_APN": args.apn, "ROOT_CA_PEM": ca,
        "MODEM_CA_FILE": f"mt15-{fingerprint}.pem",
        "MODEM_TEST_CA_PEM": certificate("ISRG Root X1"),
    }
    header = ["#pragma once", "// Generated locally from server/.env"]
    for name, value in settings.items():
        header.extend((f"#undef {name}", f"#define {name} {json.dumps(value)}"))
    enabled = int(args.enable_4g)
    header.extend(("#undef MODEM_ENABLED", f"#define MODEM_ENABLED {enabled}",
                   "#undef MODEM_TLS_VERIFIED", f"#define MODEM_TLS_VERIFIED {enabled}"))
    target = ROOT / "include" / "supabase.local.h"
    temporary = target.with_suffix(".tmp")
    temporary.write_text("\n".join(header) + "\n", encoding="ascii")
    temporary.replace(target)
    print(f"Firmware configuration prepared. 4G telemetry={bool(enabled)}.")


if __name__ == "__main__":
    main()
