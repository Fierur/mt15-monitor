"""Send serial commands to the ESP32 and print what it answers.

  & 'C:\\Users\\yiinn\\.platformio\\penv\\Scripts\\python.exe' scripts/serial_cmd.py --port COM3 DIAG STATUS
  ... --reset          reboot the ESP32 first and show the boot log
  ... --listen 30      only listen for 30 s (no commands)
"""
import argparse
import sys
import time

import serial


def read_for(port, seconds):
    end = time.time() + seconds
    while time.time() < end:
        data = port.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("commands", nargs="*")
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--listen", type=float, default=0)
    parser.add_argument("--wait", type=float, default=4, help="Seconds to read after each command.")
    args = parser.parse_args()
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.1
    port.dtr = port.rts = False  # opening must not reset the ESP32 or enter download mode
    port.open()
    try:
        if args.reset:
            port.rts = True
            time.sleep(0.15)
            port.rts = False
            print("=== BOOT ===")
            read_for(port, 5)
        for command in args.commands:
            print(f"\n=== {command} ===")
            port.write((command + "\n").encode())
            read_for(port, 14 if command == "DIAG" else args.wait)
        if args.listen:
            read_for(port, args.listen)
    finally:
        port.close()


if __name__ == "__main__":
    main()
