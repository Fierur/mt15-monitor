#pragma once
// Flags are #ifndef-guarded only so a compile-check build can force every branch
// (see docs/STATUS.md). Real hardware settings are changed HERE, after verification.
// Verified on hardware 28/10/2026 with the DIAG command: MPU on SDA=22/SCL=21 (WHO_AM_I 0x74 clone),
// GPS NMEA on GPIO16. DS18B20 wiring still being fixed; the firmware tolerates its absence.
#ifndef WIRING_CONFIRMED
#define WIRING_CONFIRMED 1
#endif
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 21
#define GPS_RX_PIN 16 // GPS TX -> ESP RX
#define GPS_TX_PIN 17 // ESP TX -> GPS RX
#define TEMP_PIN 4
#define BATTERY_PIN 34
// Connect GPIO34 to the midpoint of the 10k / 2.2k battery voltage divider.
#ifndef BATTERY_SENSE_ENABLED
#define BATTERY_SENSE_ENABLED 1
#endif
// GPIO23 has an unidentified wire: deliberately never driven by this firmware.
// Actual installed resistor is 10k. Change only AFTER changing hardware.
#define DIVIDER_TOP_OHMS 10000.0f
#define DIVIDER_BOTTOM_OHMS 2200.0f
#define BATTERY_GAIN 1.0f
#define BATTERY_OFFSET_V 0.0f
// Set to 1 only after the two-point multimeter calibration in docs/CALIBRATION.md.
#ifndef BATTERY_CALIBRATED
#define BATTERY_CALIBRATED 0
#endif
#ifndef IGNITION_PIN
#define IGNITION_PIN -1 // protected input, never raw battery voltage
#endif
#ifndef MPU_INT_PIN
#define MPU_INT_PIN -1 // RTC-capable GPIO, MPU INT active LOW
#endif
#define IGNITION_ACTIVE_LEVEL HIGH
// Active HIGH load-switch enables, never bare modem PWRKEY.
#ifndef MODEM_POWER_PIN
#define MODEM_POWER_PIN -1
#endif
#ifndef GPS_POWER_PIN
#define GPS_POWER_PIN -1
#endif
#ifndef CUTOFF_PIN
#define CUTOFF_PIN -1
#endif
#ifndef POWER_HARDWARE_VERIFIED
#define POWER_HARDWARE_VERIFIED 0
#endif
#ifndef LOW_POWER_ENABLED
#define LOW_POWER_ENABLED 0
#endif
#ifndef LOW_BATTERY_V
#define LOW_BATTERY_V 0.0f // disabled until battery type/threshold confirmed
#endif
#ifndef MODEM_ENABLED
#define MODEM_ENABLED 0
#endif
#define MODEM_RX_PIN 26
#define MODEM_TX_PIN 27
// Enable after wrong-host/expired/untrusted-cert rejection tests on real modem.
#ifndef MODEM_TLS_VERIFIED
#define MODEM_TLS_VERIFIED 0
#endif
#define ALLOW_LAN_HTTP 0 // bench only, private IPv4, no Internet HTTP
