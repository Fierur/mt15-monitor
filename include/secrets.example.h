#pragma once
// Copy to include/secrets.h (git-ignored) and fill in locally. Never commit or paste it in chat.
// If you already have an old secrets.h (MQTT starter), keep it and ADD these lines;
// the legacy MQTT/OTA macros are ignored by this firmware.
#define DEVICE_ID "mt15-001"          // must equal DEVICE_ID in server/.env
#define DEVICE_TOKEN ""               // must equal DEVICE_TOKEN in server/.env (>= 24 chars)
#define SERVER_URL ""                 // HTTPS base URL, no /api/v1/telemetry suffix; can be changed later with: URL https://...
#define WIFI_SSID ""                  // optional bench transport
#define SUPABASE_URL ""               // https://<project-ref>.supabase.co enables cloud ingest
#define SUPABASE_PUBLISHABLE_KEY ""   // Public key only. DEVICE_TOKEN must match supabase/device.sql.
#define WIFI_PASSWORD ""
#define CELLULAR_APN ""               // e.g. carrier APN for the A7680C SIM
#define ROOT_CA_PEM ""                // PEM root CA of the server certificate (Wi-Fi HTTPS); validation is never disabled
#define MODEM_CA_FILE "server-root.pem" // same CA, uploaded into the A7680C file system beforehand
