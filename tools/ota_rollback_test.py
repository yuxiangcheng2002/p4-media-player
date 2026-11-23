#!/usr/bin/env python3
"""Bench test of the drive updater's rollback (firmware/p4_loop/main/update.h).

With a new image in /update on the drive, this resets the P4, waits for the
updater to install it and for the new image to boot "pending verify", then
resets the chip inside the confirm window (right after its first frame, before
"UPDATE confirmed"). Expected: the bootloader rolls back to the previous image,
which installs the update again (second try); the script then lets that boot
confirm itself. Every console line is logged with a host timestamp.

Usage:
  tools/ota_rollback_test.py --port /dev/cu.usbmodemXXXX --out tests/results/.../rollback.log
"""
import argparse
import os
import sys
import time

import serial


def pulse_reset(ser):
    # esptool's hard reset on USB-Serial-JTAG
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=90)
    a = ap.parse_args()

    t0 = time.time()
    ser = None
    resets_in_window = 0
    pending = False
    buf = b""

    def reopen():
        nonlocal ser
        while time.time() - t0 < a.seconds:
            try:
                ser = serial.Serial(a.port, 115200, timeout=0.2)
                return True
            except (serial.SerialException, OSError):
                time.sleep(0.1)
        return False

    if not reopen():
        sys.exit(f"port {a.port} never appeared")
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "w") as log:
        def note(msg):
            line = f"{time.time() - t0:8.3f}  ### {msg}"
            log.write(line + "\n"); log.flush(); print(line)

        note("reset to start")
        pulse_reset(ser)
        while time.time() - t0 < a.seconds:
            try:
                chunk = ser.read(4096)
            except (serial.SerialException, OSError):
                ser.close()
                if not reopen():
                    break
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                log.write(f"{time.time() - t0:8.3f}  {text}\n"); log.flush()
                if any(k in text for k in ("UPDATE", "update:", "App version", "reset reason", "FIRST_FRAME",
                                           "rollback", "ota", "Loaded app")):
                    print(f"{time.time() - t0:8.3f}  {text}")
                if "UPDATE pending verify" in text:
                    pending = True
                elif pending and "FIRST_FRAME" in text and resets_in_window == 0:
                    note("reset inside the confirm window")
                    resets_in_window += 1
                    pending = False
                    pulse_reset(ser)
                elif "UPDATE confirmed" in text:
                    note("confirmed - letting it run")
    if ser:
        ser.close()


if __name__ == "__main__":
    main()
