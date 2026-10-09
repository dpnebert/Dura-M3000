#!/usr/bin/env python3
"""Hold a serial port open with selected DTR/RTS states for ESP32 EN/BOOT debugging."""
from __future__ import annotations

import argparse
import time



def state(value: str) -> bool | None:
    value = value.lower()
    if value in {"unchanged", "none"}:
        return None
    if value in {"1", "true", "on", "assert", "asserted", "high"}:
        return True
    if value in {"0", "false", "off", "deassert", "deasserted", "low"}:
        return False
    raise argparse.ArgumentTypeError("use unchanged, assert/1/on, or deassert/0/off")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--dtr", type=state, default=None)
    parser.add_argument("--rts", type=state, default=None)
    parser.add_argument("--seconds", type=float, default=15.0)
    args = parser.parse_args()

    try:
        import serial  # type: ignore[import-not-found]
    except ImportError:
        print("pyserial is required. Install with: python -m pip install pyserial")
        return 2

    ser = serial.Serial(args.port, args.baud, timeout=0)
    try:
        if args.dtr is not None:
            ser.dtr = args.dtr
        if args.rts is not None:
            ser.rts = args.rts
        print(f"Holding {args.port} open for {args.seconds:g}s with DTR={ser.dtr} RTS={ser.rts}")
        print("Watch the display/EN/3V3. Ctrl+C to stop.")
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            time.sleep(0.1)
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())

