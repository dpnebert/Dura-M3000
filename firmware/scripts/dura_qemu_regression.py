#!/usr/bin/env python3
"""Run Dura M3000 ESP32-S3 QEMU local legacy-meter regression tests."""
from __future__ import annotations

import argparse
import os
import re
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_COMMANDS = [
    ("help", r"Dura M3000 local meter commands:"),
    ("whoami", r'"firmware".*"network_node_ready":false'),
    ("provision_info", r'"state":"unprovisioned".*"standalone_ready":true.*"network_node_ready":false.*"local_liquid_id":0'),
    ("liquid_list", r'"selected":"unassigned".*"selected_id":0.*"id":17.*"name":"Water 66F".*"id":7.*"name":"10W Oil 71F"'),
    ("liquid_select 17", r'ok: liquid selected'),
    ("whoami", r'"local_liquid":"Water 66F".*"local_liquid_id":17.*"provisioning_state":"configured".*"network_node_ready":false'),
    ("liquid_list", r'"selected":"Water 66F".*"selected_id":17.*"id":17.*"selected":true'),
    ("meter_status", r'"pump":false'),
    ("meter_config", r'"batch_preset_gal"'),
    ("sim_pulse 3", r'ok: pulse recorded'),
    ("meter_status", r'"counts":\{"meter":3'),
    ("sanity_check", r'"sanity_check".*"PASS"'),
    ("run_tests 1", r'"test_report":\{"status":"PASS"'),
]


def run(cmd: list[str], *, cwd: Path, timeout: int) -> None:
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True, timeout=timeout)


def read_until(proc: subprocess.Popen[bytes], pattern: str, timeout: float, transcript: list[str]) -> str:
    assert proc.stdout is not None
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout
    buf = ""
    regex = re.compile(pattern, re.DOTALL)
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited early with {proc.returncode}; tail={buf[-2000:]}")
        remaining = max(0.05, deadline - time.monotonic())
        for key, _ in sel.select(min(0.25, remaining)):
            try:
                raw = os.read(key.fileobj.fileno(), 4096)
            except BlockingIOError:
                continue
            if not raw:
                continue
            chunk = raw.decode("utf-8", errors="replace")
            buf += chunk
            transcript.append(chunk)
            sys.stdout.write(chunk)
            sys.stdout.flush()
            if regex.search(buf):
                return buf
    raise TimeoutError(f"Timed out waiting for {pattern!r}; tail={buf[-2000:]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build-qemu-regression")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--startup-timeout", type=float, default=40.0)
    parser.add_argument("--command-timeout", type=float, default=15.0)
    args = parser.parse_args()

    env_cmd = ["bash", "-lc"]
    idf_env = (
        "export PATH=/home/omi/.espressif/python_env/idf6.0_py3.11_env/bin:$PATH && "
        "source /home/omi/esp/esp-idf-v6.0.2-env.sh >/dev/null"
    )
    build_dir = args.build_dir
    defaults = "sdkconfig.defaults;sdkconfig.qemu.defaults"
    if not args.skip_build:
        run([sys.executable, "scripts/test_dura_legacy_protocol.py"], cwd=PROJECT, timeout=30)
        sdkconfig_path = PROJECT / build_dir / "sdkconfig"
        if sdkconfig_path.exists():
            sdkconfig_path.unlink()
        run(env_cmd + [f"{idf_env} && idf.py -B {build_dir} -DSDKCONFIG={build_dir}/sdkconfig -DSDKCONFIG_DEFAULTS='{defaults}' -DIDF_TARGET=esp32s3 build"], cwd=PROJECT, timeout=600)

    qemu_cmd = env_cmd + [f"{idf_env} && idf.py -B {build_dir} qemu"]
    transcript: list[str] = []
    proc = subprocess.Popen(
        qemu_cmd,
        cwd=PROJECT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    try:
        assert proc.stdout is not None
        os.set_blocking(proc.stdout.fileno(), False)
        read_until(
            proc,
            r"battery measurement unavailable.*Dura M3000 serial console ready",
            args.startup_timeout,
            transcript,
        )
        for command, expected in DEFAULT_COMMANDS:
            print(f"\n>>> {command}", flush=True)
            assert proc.stdin is not None
            proc.stdin.write((command + "\n").encode("utf-8"))
            proc.stdin.flush()
            read_until(proc, expected + r".*?\r?\n> ?", args.command_timeout, transcript)
        print("\nDURA_QEMU_REGRESSION_PASS", flush=True)
        return 0
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        out_dir = PROJECT / build_dir
        out_dir.mkdir(exist_ok=True)
        (out_dir / "qemu-regression-transcript.log").write_text("".join(transcript), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
