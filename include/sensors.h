#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
void sensorsBegin();
void sensorsPoll();
void sensorsWrite(JsonDocument &doc);
bool sensorMotion();
bool ignitionOn();
bool ignitionKnown();
float batteryVoltage();
bool gpsFresh();
bool prepareMotionWake();
void sensorsSetMotionLog(bool enabled);
void sensorsPrintStatus(Print &out);
void sensorsMpuDump(Print &out);
float sensorsTilt();        // signed left/right lean in degrees (+ left, − right), NAN if unknown
bool sensorFallen();        // tilted beyond FALL_TILT_DEG for FALL_HOLD_MS
bool sensorsTiltZero();     // store the current orientation as "upright"; false if the bike is moving
float sensorsSpeedKmh();    // GPS speed, NAN without a fresh fix
