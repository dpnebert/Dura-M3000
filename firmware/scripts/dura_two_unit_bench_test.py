#!/usr/bin/env python3
"""Retained network-profile two-unit Dura M3000 ESP32-S3 operator runner.

Not applicable to the normal CONFIG_DURA_APP_M3000_LEGACY_ONLY=y profile:
that profile intentionally disables ESP-NOW. This hardware/operator tool is not
a host regression or authorization to enable networking, open ports or run loads.

Automates the UART command sequence Daniel has been running by hand:
  1. single-unit sanity on both units,
  2. configure First/Second identities,
  3. two-unit NOW regression via run_tests 2,
  4. TestMix1 sequential recipe flow.

Default recipe completion uses sim_pulse so the suite is repeatable without a
flow fixture. Pass --manual-pulses to pause for real reed/meter pulses instead.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import select
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_LOG_DIR = PROJECT / "docs" / "test-runs"
try:
    import serial  # type: ignore[import-not-found]
except ImportError:  # pragma: no cover - exercised on hosts without pyserial
    serial = None


def posix_baud_map() -> dict[int, int]:
    import termios

    return {
        9600: termios.B9600,
        19200: termios.B19200,
        38400: termios.B38400,
        57600: termios.B57600,
        115200: termios.B115200,
        230400: termios.B230400,
        460800: termios.B460800,
        921600: termios.B921600,
    }



class BenchFailure(RuntimeError):
    pass


@dataclass
class SerialUnit:
    label: str
    port: str
    baud: int
    dtr: bool | None = None
    rts: bool | None = None
    fd: int | None = None
    ser: Any | None = None
    transcript: list[str] = field(default_factory=list)

    def open(self) -> None:
        if serial is not None:
            self.ser = serial.Serial(self.port, self.baud, timeout=0, write_timeout=2)
            # Keep auto-reset/control lines deasserted. Arduino Serial Monitor and
            # some USB-UART adapters toggle DTR/RTS on open/close; on ESP32 boards
            # those lines often feed EN/BOOT and can make the target reset or look
            # powered down when a terminal closes.
            try:
                if self.dtr is not None:
                    self.ser.dtr = self.dtr
                if self.rts is not None:
                    self.ser.rts = self.rts
            except Exception:
                pass
            time.sleep(0.1)
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
            return

        if os.name != "posix":
            raise RuntimeError("pyserial is required on this platform. Install with: py -m pip install pyserial")

        import termios

        baud_map = posix_baud_map()
        if self.baud not in baud_map:
            raise ValueError(f"Unsupported baud {self.baud}; add it to posix_baud_map")
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0  # iflag
        attrs[1] = 0  # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0  # lflag
        attrs[4] = baud_map[self.baud]
        attrs[5] = baud_map[self.baud]
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self) -> None:
        if self.ser is not None:
            self.ser.close()
            self.ser = None
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def write_line(self, command: str) -> None:
        data = (command + "\n").encode("utf-8")
        if self.ser is not None:
            self.ser.write(data)
            self.ser.flush()
        elif self.fd is not None:
            os.write(self.fd, data)
        else:
            raise RuntimeError(f"{self.label} serial is not open")
        self.transcript.append(f"\n>>> {command}\n")

    def read_available(self) -> str:
        chunks: list[str] = []
        if self.ser is not None:
            while True:
                waiting = self.ser.in_waiting
                if waiting <= 0:
                    break
                raw = self.ser.read(waiting)
                if not raw:
                    break
                text = raw.decode("utf-8", errors="replace")
                chunks.append(text)
                self.transcript.append(text)
            return "".join(chunks)

        if self.fd is None:
            return ""
        while True:
            try:
                raw = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            if not raw:
                break
            text = raw.decode("utf-8", errors="replace")
            chunks.append(text)
            self.transcript.append(text)
        return "".join(chunks)

    def wait_readable(self, timeout: float) -> bool:
        if self.ser is not None:
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if self.ser.in_waiting > 0:
                    return True
                time.sleep(min(0.02, max(0.0, end - time.monotonic())))
            return False
        if self.fd is None:
            return False
        ready, _, _ = select.select([self.fd], [], [], timeout)
        return bool(ready)

    def read_until(self, pattern: str, timeout: float, *, echo: bool = True) -> str:
        if self.ser is None and self.fd is None:
            raise RuntimeError(f"{self.label} serial is not open")
        regex = re.compile(pattern, re.DOTALL)
        deadline = time.monotonic() + timeout
        buf = ""
        while time.monotonic() < deadline:
            if self.wait_readable(min(0.2, max(0.0, deadline - time.monotonic()))):
                chunk = self.read_available()
                buf += chunk
                if echo and chunk:
                    for line in chunk.splitlines(True):
                        sys.stdout.write(f"[{self.label}] {line}")
                    sys.stdout.flush()
                if regex.search(buf):
                    return buf
        raise TimeoutError(f"{self.label}: timed out waiting for {pattern!r}; tail={buf[-2000:]}")

    def drain(self, quiet_time: float = 0.25, timeout: float = 2.0) -> str:
        """Drain stale asynchronous boot/peer logs before sending a command."""
        deadline = time.monotonic() + timeout
        quiet_deadline = time.monotonic() + quiet_time
        buf = ""
        while time.monotonic() < deadline:
            if self.wait_readable(0.05):
                buf += self.read_available()
                quiet_deadline = time.monotonic() + quiet_time
            elif time.monotonic() >= quiet_deadline:
                break
        return buf

    def command(self, command: str, expect: str = r"\r?\n> ", timeout: float = 10.0) -> str:
        print(f"\n[{self.label}] >>> {command}", flush=True)
        self.drain(quiet_time=0.15, timeout=1.0)
        self.write_line(command)
        return self.read_until(expect, timeout)

    def save_transcript(self, path: Path) -> None:
        path.write_text("".join(self.transcript), encoding="utf-8")


def extract_json_objects(text: str) -> list[Any]:
    """Extract JSON objects from noisy UART logs using brace balancing."""
    objs: list[Any] = []
    depth = 0
    start: int | None = None
    in_string = False
    escape = False
    for idx, ch in enumerate(text):
        if start is None:
            if ch == "{":
                start = idx
                depth = 1
                in_string = False
                escape = False
            continue
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                candidate = text[start:idx + 1]
                try:
                    objs.append(json.loads(candidate))
                except json.JSONDecodeError:
                    pass
                start = None
    return objs


def find_json(text: str, key: str) -> dict[str, Any]:
    for obj in reversed(extract_json_objects(text)):
        if isinstance(obj, dict) and key in obj:
            return obj
    raise BenchFailure(f"Expected JSON object containing key {key!r}; tail={text[-1200:]}")


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise BenchFailure(message)
    print(f"PASS: {message}")


def run_checked_json(unit: SerialUnit, command: str, key: str, timeout: float = 12.0) -> dict[str, Any]:
    # Wait for the expected JSON key, not merely any prompt. Boot/peer logs can
    # contain stray prompts, especially while both units are coming up together.
    out = unit.command(command, expect=rf'"{re.escape(key)}".*?\r?\n> ', timeout=timeout)
    return find_json(out, key)


def run_test(args: argparse.Namespace) -> int:
    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")

    first = SerialUnit("First", args.first_port, args.baud, dtr=args.dtr, rts=args.rts)
    second = SerialUnit("Second", args.second_port, args.baud, dtr=args.dtr, rts=args.rts)
    units = [first, second]
    try:
        for unit in units:
            unit.open()

        if args.wait_for_console:
            for unit in units:
                print(f"Waiting for {unit.label} console...", flush=True)
                # Do not accept an early "> " prompt from peer/boot chatter; the
                # command processor is ready only after this banner.
                unit.read_until(r"Dura serial console ready", args.startup_timeout)
                unit.read_until(r"\r?\n> ", 5.0)
                unit.drain(quiet_time=0.5, timeout=3.0)
        else:
            # Drain any stale monitor text.
            time.sleep(0.2)
            for unit in units:
                unit.drain(quiet_time=0.5, timeout=3.0)

        # Step 1: standalone sanity on both units.
        print("\n=== STEP 1: single-unit sanity ===", flush=True)
        for unit in units:
            whoami = run_checked_json(unit, "whoami", "firmware")
            assert_true(whoami.get("firmware") == "0.1.0", f"{unit.label} whoami returned firmware")
            meter = run_checked_json(unit, "meter_status", "screen")
            assert_true(meter.get("screen") == "home", f"{unit.label} meter screen is home")
            unit.command("button_diag", expect=r"ok: button gpio diagnostics dumped to log.*?\r?\n> ", timeout=15.0)
            sanity0 = run_checked_json(unit, "sanity_check 0", "sanity_check")
            assert_true(sanity0["sanity_check"].get("status") == "PASS", f"{unit.label} sanity_check 0 PASS")
            tests1 = run_checked_json(unit, "run_tests 1", "test_report", timeout=25.0)
            report1 = tests1["test_report"]
            assert_true(report1.get("status") == "PASS" and report1.get("failed") == 0,
                        f"{unit.label} run_tests 1 PASS")

        # Step 2: configure identities and prove peer view on both units.
        print("\n=== STEP 2: configure identities and NOW sanity ===", flush=True)
        first.command("set_liquid First 1.0", expect=r"ok: liquid set.*?\r?\n> ", timeout=10.0)
        second.command("set_liquid Second 1.0", expect=r"ok: liquid set.*?\r?\n> ", timeout=10.0)
        time.sleep(args.peer_settle)
        for unit, identity, peer_identity in [(first, "First", "Second"), (second, "Second", "First")]:
            whoami = run_checked_json(unit, "whoami", "firmware")
            assert_true(whoami.get("local_liquid") == identity, f"{unit.label} local identity is {identity}")
            peer = run_checked_json(unit, "peer_status", "peer_count")
            peer_text = json.dumps(peer)
            assert_true(peer.get("peer_count", 0) >= 1 and peer_identity in peer_text,
                        f"{unit.label} sees peer identity {peer_identity}")
            sanity1 = run_checked_json(unit, "sanity_check 1", "sanity_check")
            assert_true(sanity1["sanity_check"].get("status") == "PASS", f"{unit.label} sanity_check 1 PASS")

        # Step 3: two-unit automated NOW regression on First.
        print("\n=== STEP 3: run_tests 2 NOW regression ===", flush=True)
        first.command("recipe_clear", expect=r"ok: recipe cleared.*?\r?\n> ", timeout=10.0)
        report2 = run_checked_json(first, "run_tests 2", "test_report", timeout=30.0)["test_report"]
        assert_true(report2.get("status") == "PASS" and report2.get("failed") == 0 and report2.get("peer_recipe_started") is True,
                    "First run_tests 2 PASS with peer_recipe_started=true")
        # Drain Second logs long enough to capture peer start/abort/ack evidence.
        try:
            second.read_until(r"RECIPE_START|RECIPE_ABORT|\r?\n> ", 3.0)
        except TimeoutError:
            pass

        # Step 4: TestMix1 recipe sequence. Default uses sim_pulse for repeatability.
        print("\n=== STEP 4: TestMix1 sequential recipe ===", flush=True)
        first.command("recipe_clear", expect=r"ok: recipe cleared.*?\r?\n> ", timeout=10.0)
        first.command("test_recipe TestMix1", expect=r"created and started|started|\r?\n> ", timeout=20.0)
        detail = run_checked_json(first, "recipe_detail", "recipe")
        detail_text = json.dumps(detail)
        assert_true("First" in detail_text and "Second" in detail_text, "TestMix1 contains First and Second")
        status = run_checked_json(first, "coordinator_status", "state")
        assert_true(status.get("state") in {"leader_running", "leader_waiting_ack", "leader_waiting_peer", "idle"},
                    "First coordinator responded after TestMix1 start")

        if args.manual_pulses:
            input("Complete First seq 1 with real reed pulses, then press Enter...")
            first.command("meter_status", timeout=10.0)
        else:
            first.command(f"sim_pulse {args.pulses}", timeout=15.0)

        # Second should receive the sequence-2 recipe start after First completes.
        second.read_until(r"\[peer->local\].*RECIPE_START.*recipe=TestMix1|follower recipe=TestMix1", args.recipe_timeout)
        second.command("coordinator_status", timeout=10.0)
        if args.manual_pulses:
            input("Complete Second seq 2 with real reed pulses, then press Enter...")
            second.command("meter_status", timeout=10.0)
        else:
            second.command(f"sim_pulse {args.pulses}", timeout=15.0)

        final = run_checked_json(first, "coordinator_status", "state", timeout=20.0)
        assert_true(final.get("state") == "idle" and final.get("active_recipe", "") == "",
                    "First coordinator returned idle after TestMix1 completion")

        print("\nDURA_TWO_UNIT_BENCH_TEST_PASS", flush=True)
        return 0
    finally:
        for unit in units:
            path = log_dir / f"{stamp}-{unit.label.lower()}-two-unit-bench.log"
            unit.save_transcript(path)
            unit.close()
            print(f"Saved {unit.label} transcript: {path}", flush=True)


def parse_line_state(value: str) -> bool | None:
    lowered = value.lower()
    if lowered in {"unchanged", "none"}:
        return None
    if lowered in {"1", "true", "on", "assert", "asserted", "high"}:
        return True
    if lowered in {"0", "false", "off", "deassert", "deasserted", "low"}:
        return False
    raise argparse.ArgumentTypeError("use unchanged, assert/1/on, or deassert/0/off")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--first-port", required=True, help="Serial port for unit configured as First, e.g. /dev/ttyACM0")
    parser.add_argument("--second-port", required=True, help="Serial port for unit configured as Second, e.g. /dev/ttyACM1")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--dtr", type=parse_line_state, default=None, help="pyserial DTR state after open: unchanged, assert/1/on, or deassert/0/off")
    parser.add_argument("--rts", type=parse_line_state, default=None, help="pyserial RTS state after open: unchanged, assert/1/on, or deassert/0/off")
    parser.add_argument("--log-dir", default=str(DEFAULT_LOG_DIR))
    parser.add_argument("--startup-timeout", type=float, default=45.0)
    parser.add_argument("--command-timeout", type=float, default=12.0)
    parser.add_argument("--recipe-timeout", type=float, default=25.0)
    parser.add_argument("--peer-settle", type=float, default=2.0)
    parser.add_argument("--pulses", type=int, default=1000, help="sim_pulse count used to complete each 1.00-unit test ingredient")
    parser.add_argument("--manual-pulses", action="store_true", help="Pause for real reed/meter pulses instead of using sim_pulse")
    parser.add_argument("--no-wait-for-console", dest="wait_for_console", action="store_false", help="Do not wait for boot banner/prompt; useful if both units are already sitting at prompts")
    parser.set_defaults(wait_for_console=True)
    args = parser.parse_args()
    try:
        return run_test(args)
    except (BenchFailure, TimeoutError, OSError, KeyboardInterrupt) as exc:
        print(f"\nDURA_TWO_UNIT_BENCH_TEST_FAIL: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())



