#!/usr/bin/env python3
"""Fail-closed one-shot UART JSON ping probe; the development reboot aid is retired.

This does not add a firmware UART ping command: targets without a JSON pong fail
closed. Generic firmware BLE ping remains a separate, unchanged transport.
"""
from __future__ import annotations

import argparse
import base64
import json
import math
import time
from typing import Iterable, Sequence

VID = 0x303A
PID = 0x1002
SERIAL_NUMBER = "206EF1ABC344"
MAX_RAW_RX = 4096
EVIDENCE_SCHEMA = "m3000-uart-ping-evidence/v1"


class BenchError(RuntimeError):
    pass


def _matching_targets(ports: Iterable[object]) -> list[object]:
    return [p for p in ports if getattr(p, "vid", None) == VID
            and getattr(p, "pid", None) == PID
            and getattr(p, "serial_number", None) == SERIAL_NUMBER]


def select_target(ports: Iterable[object]) -> str:
    matches = _matching_targets(ports)
    if len(matches) != 1:
        raise BenchError(f"expected exactly one M3000 target, found {len(matches)}")
    return str(matches[0].device)


def open_serial_safely(serial_obj: object, port: str) -> object:
    if getattr(serial_obj, "is_open", False):
        raise BenchError("serial object must be closed before configuration")
    serial_obj.rtscts = False
    serial_obj.dsrdtr = False
    serial_obj.xonxoff = False
    serial_obj.dtr = False
    serial_obj.rts = False
    serial_obj.port = port
    serial_obj.open()
    serial_obj.rtscts = False
    serial_obj.dsrdtr = False
    serial_obj.xonxoff = False
    serial_obj.dtr = False
    serial_obj.rts = False
    return serial_obj


def parse_json_line(lines: Sequence[str]) -> dict:
    objects = []
    for line in lines:
        text = line.strip()
        if not (text.startswith("{") and text.endswith("}")):
            continue
        try:
            objects.append(json.loads(text))
        except json.JSONDecodeError:
            continue
    if len(objects) != 1:
        raise BenchError(f"expected exactly one JSON response, found {len(objects)}")
    return objects[0]


class BenchClient:
    def __init__(self, timeout: float = 8.0):
        if not math.isfinite(timeout) or timeout <= 0:
            raise BenchError("timeout must be finite and positive")
        try:
            import serial
            from serial.tools import list_ports
        except ImportError as exc:
            raise BenchError("pyserial is required") from exc
        self._serial_mod = serial
        self._list_ports = list_ports
        self.timeout = timeout
        self.ser = None
        self.transcript: list[str] = []
        self._clock = time
        self._init_evidence()

    def _init_evidence(self) -> None:
        self.evidence = {"schema": EVIDENCE_SCHEMA, "events": [],
                         "write_counts": {"ping": 0, "total": 0},
                         "terminal_verdict": "in_progress"}

    def _record(self, event: str, **fields: object) -> None:
        item = {"sequence": len(self.evidence["events"]),
                "monotonic_s": round(self._clock.monotonic(), 9), "event": event}
        item.update(fields)
        self.evidence["events"].append(item)

    def _fail(self, verdict: str, message: str) -> None:
        self.evidence["terminal_verdict"] = verdict
        self._record("terminal_failure", verdict=verdict, message=message)
        raise BenchError(message)

    def open(self) -> None:
        matches = _matching_targets(self._list_ports.comports())
        self._record("usb_enumeration", matched_count=len(matches))
        if len(matches) != 1:
            self._fail("target_missing" if not matches else "ambiguous_target_rejected",
                       f"expected exactly one M3000 target, found {len(matches)}")
        self.ser = self._serial_mod.Serial(
            port=None, baudrate=115200, timeout=0.1, write_timeout=1.0
        )
        # Keep ownership even if open/control-line configuration raises, so the
        # caller's finally/close can release a partially opened handle.
        open_serial_safely(self.ser, str(matches[0].device))
        self._record("serial_open", port=str(matches[0].device))

    def close(self) -> None:
        if self.ser is not None and self.ser.is_open:
            self.ser.close()
            self._record("serial_close")

    def _read_until_prompt(self) -> list[str]:
        deadline = self._clock.monotonic() + self.timeout
        data = bytearray()
        total = 0
        while self._clock.monotonic() < deadline:
            chunk = self.ser.read(min(self.ser.in_waiting or 1, MAX_RAW_RX))
            if not chunk:
                continue
            total += len(chunk)
            room = MAX_RAW_RX - len(data)
            data.extend(chunk[:room])
            if len(chunk) > room:
                self._record_rx("rx_overflow", data, total, True)
                self._fail("command_response_too_large", "console response exceeds receive limit")
            if b"\r\n> " in data or b"\n> " in data:
                text = data.decode("utf-8", errors="replace")
                self.transcript.append(text)
                self._record_rx("rx_complete", data, total, False)
                return text.splitlines()
        self._record_rx("rx_timeout", data, total, False)
        self._fail("command_timeout", "timeout waiting for complete console response")

    def _record_rx(self, event: str, data: bytes, total: int, truncated: bool) -> None:
        self._record(event, raw_rx_encoding="base64",
                     raw_rx_b64=base64.b64encode(data).decode("ascii"),
                     raw_rx_bytes=total, raw_rx_truncated=truncated)

    def command(self, command: str) -> dict:
        # Validate before touching state, enumerating, opening or sending anything.
        # Exact matching also prevents newline injection of a retired command.
        if command != "ping":
            raise BenchError("only ping is supported; UART reboot/restart aid is retired")
        try:
            if self.ser is None or not self.ser.is_open:
                self.open()
            self.ser.reset_input_buffer()
            payload = b"ping\n"
            written = self.ser.write(payload)
            self.evidence["write_counts"]["ping"] += 1
            self.evidence["write_counts"]["total"] += 1
            self._record("serial_write", command="ping", byte_count=written)
            if written != len(payload):
                self._fail("command_short_write", "incomplete ping write; not retrying")
            self.ser.flush()
            lines = self._read_until_prompt()
            try:
                response = parse_json_line(lines)
                if response.get("pong") is not True:
                    raise BenchError("ping response lacks boolean true pong")
            except BenchError as exc:
                self._fail("command_invalid_response", str(exc))
            self.evidence["terminal_verdict"] = "success"
            self._record("terminal_success")
            return response
        except OSError as exc:
            self._fail("command_io_error", f"I/O error: {exc}")

    def reboot(self) -> dict:
        """Compatibility rejection only: never inspect or touch the transport."""
        raise BenchError("UART reboot aid is retired; no serial operation performed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["ping"])
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()
    client = None
    try:
        client = BenchClient(args.timeout)
        result = client.command("ping")
        print(json.dumps(result, sort_keys=True))
        return 0
    except (BenchError, OSError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, sort_keys=True))
        return 2
    finally:
        if client is not None:
            client.close()


if __name__ == "__main__":
    raise SystemExit(main())
