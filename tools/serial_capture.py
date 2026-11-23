#!/usr/bin/env python3
"""Capture the P4's USB-Serial-JTAG console to a file, with an optional reset first.

The P4's USB-Serial-JTAG port re-enumerates when the chip resets, so the
reader reopens the port whenever it disappears instead of giving up. Every
line is written to the log with a host timestamp (seconds since capture start)
so boot milestones can be lined up with filmed video later.

Usage:
  tools/serial_capture.py --port /dev/cu.usbmodemXXXX --seconds 30 \
      --out tests/results/01-p4-mjpeg-throughput/<date>-<case>/serial.log [--reset] [--send s]
"""
import argparse
import os
import sys
import time

import serial  # pyserial (present in the ESP-IDF python env)


def open_port(port, deadline):
    while time.time() < deadline:
        try:
            return serial.Serial(port, 115200, timeout=0.2)
        except (serial.SerialException, OSError):
            time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--out", required=True)
    ap.add_argument("--reset", action="store_true", help="pulse RTS to reset the chip first")
    ap.add_argument("--send", default="", help="characters to send after 3 s (console keys)")
    ap.add_argument("--keys", default="", help="timed console keys, e.g. '20:n,40:f' (seconds:key)")
    a = ap.parse_args()

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    t0 = time.time()
    end = t0 + a.seconds
    ser = open_port(a.port, end)
    if ser is None:
        sys.exit(f"port {a.port} never appeared")
    if a.reset:
        # Same sequence esptool uses for a hard reset on USB-Serial-JTAG.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    sent = not a.send
    keys = sorted((float(t), k) for t, k in (x.split(":", 1) for x in a.keys.split(",") if x))
    buf = b""
    with open(a.out, "w") as log:
        while time.time() < end:
            try:
                chunk = ser.read(4096)
            except (serial.SerialException, OSError):
                ser.close()
                ser = open_port(a.port, end)
                if ser is None:
                    break
                continue
            try:
                if not sent and time.time() - t0 > 3:
                    ser.write(a.send.encode())
                    sent = True
                while keys and time.time() - t0 >= keys[0][0]:
                    ser.write(keys[0][1].encode())
                    keys.pop(0)
            except (serial.SerialException, OSError):
                pass  # port went away (chip reset); the read above reopens it
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                stamped = f"{time.time() - t0:8.3f}  {text}"
                log.write(stamped + "\n")
                log.flush()
                print(stamped)
    if ser:
        ser.close()


if __name__ == "__main__":
    main()
