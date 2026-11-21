"""Forward one MT15 device's MQTT telemetry to its existing Supabase RPC."""

import argparse
import json
import logging
import re
from pathlib import Path

import httpx
from dotenv import dotenv_values
from paho.mqtt import client as mqtt


PROJECT = Path(__file__).resolve().parents[1]
PERMANENT_ERRORS = (400, 413, 422)
DEVICE_PATTERN = r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}"
BOOT_PATTERN = r"[A-Za-z0-9_-]{1,64}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-env", type=Path, required=True)
    args = parser.parse_args()
    project = dotenv_values(PROJECT / "server" / ".env")
    broker = dotenv_values(args.mqtt_env)
    device_id = project.get("DEVICE_ID") or "mt15-001"
    token = project.get("DEVICE_TOKEN") or ""
    url = (project.get("SUPABASE_URL") or "").rstrip("/")
    key = project.get("SUPABASE_PUBLISHABLE_KEY") or project.get("SUPABASE_ANON_KEY") or ""
    username = broker.get("MQTT_BRIDGE_USER") or ""
    password = broker.get("MQTT_BRIDGE_PASSWORD") or ""
    port = int(broker.get("MQTT_LOCAL_PORT") or "1887")
    if not re.fullmatch(DEVICE_PATTERN, device_id) or len(token) < 24:
        parser.error("DEVICE_ID or DEVICE_TOKEN is missing or invalid in server/.env")
    if not re.fullmatch(r"https://[a-z0-9-]+\.supabase\.co", url) or not key.startswith("sb_publishable_"):
        parser.error("Supabase URL or publishable key is missing or invalid in server/.env")
    if not username or not password or not 1 <= port <= 65535:
        parser.error("MQTT bridge credentials or loopback port are missing or invalid")

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    telemetry_topic = f"mt15/{device_id}/telemetry"
    ack_topic = f"mt15/{device_id}/ack"
    broker_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"bridge-{device_id}")
    broker_client.username_pw_set(username, password)
    broker_client.reconnect_delay_set(min_delay=1, max_delay=15)

    with httpx.Client(base_url=url, headers={"apikey": key}, timeout=3.0) as http:
        def on_connect(client, _userdata, _flags, code, _properties):
            if code.is_failure:
                logging.error("Local MQTT broker rejected the bridge: %s", code)
                return
            client.subscribe(telemetry_topic, qos=1)

        def on_subscribe(_client, _userdata, _mid, codes, _properties):
            if codes and all(not code.is_failure for code in codes):
                logging.info("Listening for MT15 telemetry on the local broker")
            else:
                logging.error("Local MQTT broker refused the telemetry subscription")

        def on_message(client, _userdata, message):
            if message.topic != telemetry_topic or message.retain or len(message.payload) >= 1536:
                return
            try:
                payload = json.loads(message.payload)
            except (UnicodeDecodeError, json.JSONDecodeError):
                return
            if not isinstance(payload, dict) or payload.get("device_id") != device_id:
                return
            boot = payload.get("boot_id")
            seq = payload.get("seq")
            if not isinstance(boot, str) or not re.fullmatch(BOOT_PATTERN, boot) or type(seq) is not int or not 0 <= seq <= 0xffffffff:
                return
            expected_ack = {"boot_id": boot, "seq": seq}
            try:
                response = http.post("/rest/v1/rpc/ingest_telemetry", json={
                    "p_device_id": device_id, "p_token": token, "p_payload": payload,
                })
                if response.status_code == 200:
                    reply = response.json()
                    if not isinstance(reply, dict) or reply.get("accepted") is not True or reply.get("ack") != expected_ack:
                        logging.warning("Supabase returned an invalid acknowledgement")
                        return
                elif response.status_code in PERMANENT_ERRORS:
                    reply = {"ack": expected_ack}
                else:
                    logging.warning("Supabase returned HTTP %s; device will retry", response.status_code)
                    return
            except (httpx.HTTPError, ValueError) as error:
                logging.warning("Supabase request failed: %s", type(error).__name__)
                return

            reply["status"] = response.status_code
            result = client.publish(ack_topic, json.dumps(reply, separators=(",", ":")), qos=1)
            if result.rc != mqtt.MQTT_ERR_SUCCESS:
                logging.warning("Broker could not queue the acknowledgement")

        broker_client.on_connect = on_connect
        broker_client.on_subscribe = on_subscribe
        broker_client.on_message = on_message
        broker_client.connect_async("127.0.0.1", port, keepalive=45)
        broker_client.loop_forever(retry_first_connection=True)


if __name__ == "__main__":
    main()
