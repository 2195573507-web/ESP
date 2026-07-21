#!/usr/bin/env python3
"""Verify ESP32-S3 startup checkpoints across repeated hardware resets."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import serial


REQUIRED_STAGES = (
    "app_main_enter",
    "after_task_create",
    "after_gateway",
    "after_wifi",
    "after_radar",
    "after_bme",
)
FAILURE_MARKERS = (
    "Guru Meditation",
    "CORRUPT HEAP",
    "HEAP_INTEGRITY_FIRST_FAILURE",
    "stack overflow",
    "Stack canary watchpoint triggered",
)
READY_MARKER = "gateway orchestrator startup complete"


def hard_reset(port: serial.Serial) -> None:
    """Use the board's USB serial RTS line, matching esptool's hard reset."""
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    time.sleep(0.1)


def run_boot(port: serial.Serial, timeout_s: float) -> tuple[bool, str, set[str], list[str]]:
    port.reset_input_buffer()
    hard_reset(port)
    deadline = time.monotonic() + timeout_s
    lines: list[str] = []
    stages: set[str] = set()
    failures: list[str] = []
    ready = False

    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").rstrip()
        if not line:
            continue
        lines.append(line)
        for stage in REQUIRED_STAGES:
            if f"HEAP_INTEGRITY stage={stage} intact=1" in line:
                stages.add(stage)
        if READY_MARKER in line:
            ready = True
        if any(marker in line for marker in FAILURE_MARKERS):
            failures.append(line)
            break
        if ready and len(stages) == len(REQUIRED_STAGES):
            break

    complete = ready and len(stages) == len(REQUIRED_STAGES) and not failures
    return complete, "\n".join(lines), stages, failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--count", type=int, default=30)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()

    args.log.parent.mkdir(parents=True, exist_ok=True)
    failures = 0
    with serial.Serial(args.port, 115200, timeout=0.2) as port, args.log.open("w", encoding="utf-8") as log:
        for number in range(1, args.count + 1):
            complete, output, stages, boot_failures = run_boot(port, args.timeout)
            missing = sorted(set(REQUIRED_STAGES) - stages)
            status = "PASS" if complete else "FAIL"
            print(f"boot={number:02d} status={status} stages={len(stages)}/{len(REQUIRED_STAGES)} ready={int(READY_MARKER in output)}", flush=True)
            log.write(f"\n===== BOOT {number:02d} {status} =====\n{output}\n")
            if missing:
                log.write(f"MISSING_STAGES={','.join(missing)}\n")
            if boot_failures:
                log.write(f"FAILURE_MARKERS={' | '.join(boot_failures)}\n")
            log.flush()
            if not complete:
                failures += 1
                print(f"  missing={','.join(missing) or '-'} failures={' | '.join(boot_failures) or '-'}", flush=True)

    print(f"summary boots={args.count} passed={args.count - failures} failed={failures} log={args.log}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
