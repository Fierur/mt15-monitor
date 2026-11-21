#pragma once
#include <Arduino.h>
// Read-only wiring probes for bench bring-up. Safe while WIRING_CONFIRMED=0:
// only input/open-drain buses on the reported pins; GPIO23 is never touched.
void runDiagnostics(Print &out);
// Bench modem check on MODEM_RX_PIN/MODEM_TX_PIN: read-only AT queries, or one
// user-typed command. Refused while MODEM_ENABLED (the network task owns UART2).
// Drives MODEM_TX_PIN at 3.3 V: only after confirming the board's UART level.
void runModemTest(Print &out,const String &custom);
// Passive: measure the idle voltage on the modem's TXD (GPIO26) and RXD (GPIO27) lines with the
// ESP32 ADC, without driving anything. Tells whether the modem is powered and its UART logic level.
void runModemLevel(Print &out);
void runPinProbe(Print &out,int pin);
