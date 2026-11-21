#pragma once

// Personal settings first, defaults second.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#if __has_include("supabase.local.h")
#include "supabase.local.h"
#endif
#if __has_include("mqtt.local.h")
#include "mqtt.local.h"
#endif
#ifndef MT15_MQTT_HOST
#define MT15_MQTT_HOST ""
#endif
#ifndef MT15_MQTT_PORT
#define MT15_MQTT_PORT 443
#endif
#ifndef MT15_MQTT_USER
#define MT15_MQTT_USER ""
#endif
#ifndef MT15_MQTT_PASSWORD
#define MT15_MQTT_PASSWORD ""
#endif
#ifndef MT15_MQTT_CA_PEM
#define MT15_MQTT_CA_PEM ""
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "mt15-001"
#endif
#ifndef DEVICE_TOKEN
#define DEVICE_TOKEN ""
#endif
#ifndef SERVER_URL
#define SERVER_URL ""
#endif
#ifndef SUPABASE_URL
#define SUPABASE_URL ""
#endif
#ifndef SUPABASE_PUBLISHABLE_KEY
#define SUPABASE_PUBLISHABLE_KEY ""
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef CELLULAR_APN
#define CELLULAR_APN ""
#endif
#ifndef ROOT_CA_PEM
#define ROOT_CA_PEM ""
#endif
#ifndef MODEM_CA_FILE
#define MODEM_CA_FILE "server-root.pem"
#endif
#ifndef MODEM_TEST_CA_PEM
#define MODEM_TEST_CA_PEM ""
#endif
#include "hardware.h"
constexpr unsigned long REPORT_MS=1000;
constexpr unsigned long PARK_REPORT_SECONDS=900;
constexpr unsigned long STILL_MS=120000;
constexpr unsigned long MOTION_EVENT_MS=5000;
constexpr size_t PACKET_SIZE=1536;
constexpr size_t OUTBOX_LIMIT=120;
constexpr size_t OUTBOX_BAD_LIMIT=10;
// Sleep sequencing: leave early once the network task is idle, never stay awake forever.
constexpr unsigned long SHUTDOWN_WAIT_MS=30000;
constexpr unsigned long SHUTDOWN_HARD_MS=120000;
// After a timer wake: stay up until a fresh GPS fix and one server ACK, bounded.
constexpr unsigned long TIMER_WAKE_MIN_MS=15000;
constexpr unsigned long TIMER_WAKE_MAX_MS=90000;
// Motion detector (software). Calibrate with the MOTION_LOG serial command.
constexpr float MOTION_ACCEL_DELTA_G=0.20f;
constexpr float MOTION_GYRO_DPS=25.0f;
constexpr unsigned MOTION_SAMPLES=3;
// MPU6050 wake interrupt (hardware). MOT_THR LSB is ~2 mg; too low wakes on traffic vibration.
constexpr uint8_t MPU_WAKE_THRESHOLD=10;
constexpr uint8_t MPU_WAKE_DURATION_MS=20;
// Tip-over: angle between the current gravity vector and the reference taken with the bike upright.
// Side stand ~10-15 deg, bike lying down ~70-90 deg.
constexpr float FALL_TILT_DEG=60.0f;
// Only left/right lean counts (roll about the bike's length axis); pitch (ramps, braking) is ignored.
// Mount the GY-521 with its printed X arrow pointing to the FRONT of the bike (0=X, 1=Y, 2=Z).
constexpr int MPU_FORWARD_AXIS=0;
// +1: positive lean = left. If the website shows "phải" when the bike leans left, set -1.
constexpr int MPU_ROLL_SIGN=1;
constexpr float FALL_CLEAR_DEG=35.0f;
constexpr unsigned long FALL_HOLD_MS=3000;
// Without an ignition wire, "driving" is inferred from GPS speed (with hysteresis).
constexpr float DRIVE_SPEED_KMH=5.0f;
constexpr float STOP_SPEED_KMH=3.0f;
constexpr unsigned long DRIVE_STOP_MS=30000;

