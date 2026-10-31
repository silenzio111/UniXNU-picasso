#!/usr/bin/env python3
"""Boot one Picasso M1 image and verify the cold-boot acceptance gates."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
CAPTURE_PATH = ROOT / "scripts/capture-picasso-usb-console.py"


def load_capture():
    spec = importlib.util.spec_from_file_location("capture", CAPTURE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-image", required=True, type=Path)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--serial",
        help="required Fastboot serial; without this, probe all candidates",
    )
    parser.add_argument("--max-seconds", type=float, default=720.0)
    parser.add_argument("--post-evidence-seconds", type=float, default=20.0)
    args = parser.parse_args()

    capture = load_capture()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    events_path = output_dir / "events.log"
    cdc_path = output_dir / "hardware-cdc.log"
    result_path = output_dir / "result.json"
    started = time.monotonic()

    def event(text):
        line = f"{time.monotonic() - started:.3f}s {text}"
        print(line, flush=True)
        with events_path.open("a") as log:
            log.write(line + "\n")

    def fastboot(*fastboot_args, timeout=180):
        result = subprocess.run(
            ["fastboot", *fastboot_args],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        body = (result.stdout + result.stderr).strip()
        event(
            f"fastboot {' '.join(fastboot_args)} rc={result.returncode}: {body}"
        )
        return result

    def fastboot_present(serial):
        try:
            result = subprocess.run(
                ["fastboot", "devices", "-l"],
                capture_output=True,
                text=True,
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            return None
        return bool(
            re.search(
                rf"^{re.escape(serial)}\s+fastboot\b", result.stdout, re.M
            )
        )

    def confirmed_fastboot(serial):
        if not fastboot_present(serial):
            return False
        try:
            result = subprocess.run(
                ["fastboot", "-s", serial, "getvar", "is-userspace"],
                capture_output=True,
                text=True,
                timeout=10,
            )
        except subprocess.TimeoutExpired:
            return False
        return bool(
            result.returncode == 0
            and re.search(
                r"^is-userspace:\s*no\s*$",
                result.stdout + result.stderr,
                re.M,
            )
        )

    def getvar(serial, name):
        try:
            return subprocess.run(
                ["fastboot", "-s", serial, "getvar", name],
                capture_output=True,
                text=True,
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            return None

    def select_picasso():
        result = subprocess.run(
            ["fastboot", "devices", "-l"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        candidates = re.findall(
            r"^(\S+)\s+fastboot\b", result.stdout, re.M
        )
        event(f"Fastboot candidates: {candidates}")
        if args.serial:
            candidates = [candidate for candidate in candidates if candidate == args.serial]
        responsive = []
        for candidate in candidates:
            product = getvar(candidate, "product")
            serialno = getvar(candidate, "serialno")
            mode = getvar(candidate, "is-userspace")
            if product is None or serialno is None or mode is None:
                event(f"skip unresponsive Fastboot candidate {candidate}")
                continue
            product_body = product.stdout + product.stderr
            serial_body = serialno.stdout + serialno.stderr
            mode_body = mode.stdout + mode.stderr
            if (
                product.returncode == 0
                and re.search(r"^product:\s*picasso\s*$", product_body, re.M)
                and serialno.returncode == 0
                and re.search(
                    rf"^serialno:\s*{re.escape(candidate)}\s*$",
                    serial_body,
                    re.M,
                )
                and mode.returncode == 0
                and re.search(
                    r"^is-userspace:\s*no\s*$", mode_body, re.M
                )
            ):
                responsive.append(candidate)
            else:
                event(
                    f"skip candidate {candidate}: product/serial/mode did not "
                    "identify the responsive Picasso Fastboot"
                )
        if len(responsive) != 1:
            raise RuntimeError(
                f"expected one responsive Fastboot device, got {responsive}"
            )
        event(f"selected responsive Fastboot serial {responsive[0]}")
        return responsive[0]

    boot_image = args.boot_image.resolve()
    actual_sha256 = hashlib.sha256(boot_image.read_bytes()).hexdigest()
    if actual_sha256 != args.expected_sha256:
        raise RuntimeError(
            f"boot image hash mismatch: {actual_sha256} != "
            f"{args.expected_sha256}"
        )
    if capture.serial_ports():
        raise RuntimeError(
            f"existing CDC port before boot: {capture.serial_ports()}"
        )
    process_list = subprocess.check_output(
        ["ps", "-axo", "command"], text=True
    )
    if any(
        re.search(
            r"python\S* .*scripts/"
            r"(capture-picasso-usb-console|picasso-cdc-command)\.py",
            line,
        )
        for line in process_list.splitlines()
    ):
        raise RuntimeError("another CDC reader is already running")

    serial = select_picasso()
    mode = fastboot("-s", serial, "getvar", "is-userspace", timeout=15)
    if not (
        mode.returncode == 0
        and re.search(
            r"^is-userspace:\s*no\s*$",
            mode.stdout + mode.stderr,
            re.M,
        )
    ):
        raise RuntimeError("selected device is not real Fastboot")
    (output_dir / "controller.pid").write_text(f"{os.getpid()}\n")

    boot = fastboot("-s", serial, "boot", str(boot_image))
    if boot.returncode != 0:
        raise RuntimeError("temporary boot was rejected; refusing to retry")
    booted = time.monotonic()

    fd = None
    opened_at = None
    disappeared = False
    disappear_at = None
    disconnect_at = None
    data = bytearray()
    status_sent = False
    help_sent = False
    reboot_sent = False
    reboot_sent_at = None
    reboot_attempts = 0
    next_reboot_retry = None
    fastboot_absent_after_reboot = False
    fastboot_returned_after_reboot = False
    evidence_since = None
    last_dev_poll = 0.0

    with cdc_path.open("wb", buffering=0) as log:
        while time.monotonic() - booted < args.max_seconds:
            now = time.monotonic()
            if now - last_dev_poll >= 0.5:
                last_dev_poll = now
                present = fastboot_present(serial)
                if present is False and not disappeared:
                    disappeared = True
                    disappear_at = now - booted
                    event(f"device left Fastboot at +{disappear_at:.3f}s")
                if reboot_sent and present is False:
                    fastboot_absent_after_reboot = True
                if (
                    reboot_sent
                    and fastboot_absent_after_reboot
                    and present is True
                    and confirmed_fastboot(serial)
                ):
                    fastboot_returned_after_reboot = True
                    event("reboot-fastboot returned to confirmed Fastboot")
                    break

            if fd is None and disconnect_at is None:
                ports = capture.serial_ports()
                if len(ports) > 1:
                    event(f"multiple CDC ports: {ports}; refusing arbitrary port")
                    break
                if not ports:
                    time.sleep(0.05)
                    continue
                try:
                    fd = os.open(
                        ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK
                    )
                    capture.configure_serial(fd)
                    before, after = capture.assert_modem_lines(fd)
                    opened_at = time.monotonic()
                    event(
                        f"opened {ports[0]} DTR/RTS={before:#x}->{after:#x}"
                    )
                except OSError as error:
                    event(f"open/configure failed: {error}")
                    if fd is not None:
                        os.close(fd)
                        fd = None
                    time.sleep(0.05)
                    continue

            if fd is None:
                break

            try:
                ready, _, _ = select.select([fd], [], [], 0.05)
                if ready:
                    chunk = os.read(fd, 65536)
                    if not chunk:
                        disconnect_at = time.monotonic()
                        event("CDC EOF")
                    else:
                        log.write(chunk)
                        data.extend(chunk)
            except (OSError, ValueError) as error:
                disconnect_at = time.monotonic()
                event(f"CDC disconnected: {error}")

            text = bytes(data)
            status_ok = b"PICASSO CDC: configured=" in text
            help_ok = b"PICASSO CDC: commands: help | status |" in text
            if opened_at is not None and not status_sent:
                os.write(fd, b"status\r\n")
                status_sent = True
                event("TX status")
            elif status_ok and not help_sent:
                os.write(fd, b"help\r\n")
                help_sent = True
                event("TX help")

            timer_snapshot = b"PICASSO TIMER: snapshot complete" in text
            timer_sleep = (
                b"PICASSO TIMER: sleep summary io=10/10 aw=10/10 PASS"
                in text
            )
            timer_end = b"PICASSO TIMER: selftest end kept" in text
            timer_done = b"PICASSO TIMER: selftest done" in text
            timer_rearm = b"PICASSO TIMER: keep rearm end" in text
            timer_defer = b"PICASSO TIMER: defer post" in text and b"ok=1" in text
            control_done = b"PICASSO USB: control window complete" in text
            shared_region = b"PICASSO SR: map exit pid=1 error=0 kr=0x0" in text
            early_boot = (
                b"Early boot complete. Continuing system boot." in text
            )
            service_names = {
                name.encode()
                for name in (
                    "SpringBoard",
                    "backboardd",
                    "runningboardd",
                    "powerd",
                )
                if name.encode() in text
            }
            panic_seen = b"=== PICASSO CDC PANIC BEGIN ===" in text

            all_evidence = all(
                (
                    status_ok,
                    help_ok,
                    timer_snapshot,
                    timer_sleep,
                    timer_end,
                    timer_done,
                    timer_rearm,
                    timer_defer,
                    control_done,
                    shared_region,
                    early_boot,
                    len(service_names) == 4,
                )
            )
            if all_evidence and evidence_since is None:
                evidence_since = time.monotonic()
                event(
                    "all M1 evidence observed; "
                    f"services={sorted(name.decode() for name in service_names)}"
                )

            if (
                evidence_since is not None
                and not reboot_sent
                and time.monotonic() - evidence_since
                >= args.post_evidence_seconds
            ):
                os.write(fd, b"reboot-fastboot\r\n")
                reboot_sent = True
                reboot_sent_at = time.monotonic()
                reboot_attempts = 1
                next_reboot_retry = time.monotonic() + 15.0
                fastboot_absent_after_reboot = True
                event("TX reboot-fastboot after stable M1 evidence")
            elif (
                not reboot_sent
                and opened_at is not None
                and time.monotonic() - opened_at >= 500.0
            ):
                os.write(fd, b"reboot-fastboot\r\n")
                reboot_sent = True
                reboot_sent_at = time.monotonic()
                reboot_attempts = 1
                next_reboot_retry = time.monotonic() + 15.0
                fastboot_absent_after_reboot = True
                event("TX reboot-fastboot recovery fallback")

            if panic_seen and not reboot_sent:
                event("panic marker observed; waiting for panic reset path")
            if (
                reboot_sent
                and b"PICASSO CDC: reboot-fastboot requested" not in text
                and reboot_attempts < 3
                and next_reboot_retry is not None
                and time.monotonic() >= next_reboot_retry
            ):
                os.write(fd, b"reboot-fastboot\r\n")
                reboot_attempts += 1
                next_reboot_retry = time.monotonic() + 15.0
                event(
                    f"TX reboot-fastboot retry {reboot_attempts}/3; "
                    "no acknowledged command yet"
                )
            if (
                reboot_sent
                and reboot_sent_at is not None
                and time.monotonic() - reboot_sent_at >= 120.0
            ):
                event("reboot command had 120 s; close CDC and verify Fastboot")
                break
            if disconnect_at is not None:
                break

    if fd is not None:
        os.close(fd)

    text = bytes(data)
    timer_poll_irqs = [
        int(value)
        for value in re.findall(
            rb"PICASSO TIMER: selftest poll t=\d+ irq=(\d+)", text
        )
    ]
    sleep_passes = len(
        re.findall(
            rb"PICASSO TIMER: sleep iter=\d+ io_ms=(?:9\d|10\d|110) PASS "
            rb"aw_a=-?\d+ aw_rc=1 aw_ms=(?:9\d|10\d|110) PASS",
            text,
        )
    )

    returned = False
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        if fastboot_absent_after_reboot and confirmed_fastboot(serial):
            returned = True
            break
        time.sleep(0.25)

    result = {
        "boot_image": str(boot_image),
        "boot_sha256": actual_sha256,
        "serial": serial,
        "cdc_bytes": len(text),
        "cdc_opened_at_s": (
            opened_at - booted if opened_at is not None else None
        ),
        "disappearance_observed": disappeared,
        "disappear_at_s": disappear_at,
        "returned_real_fastboot": returned,
        "status_response": b"PICASSO CDC: configured=" in text,
        "help_response": b"PICASSO CDC: commands: help | status |" in text,
        "timer_snapshot_complete": (
            b"PICASSO TIMER: snapshot complete" in text
        ),
        "timer_sleep_passes": sleep_passes,
        "timer_sleep_summary_pass": (
            b"PICASSO TIMER: sleep summary io=10/10 aw=10/10 PASS" in text
        ),
        "timer_selftest_end_kept": (
            b"PICASSO TIMER: selftest end kept" in text
        ),
        "timer_selftest_done": b"PICASSO TIMER: selftest done" in text,
        "timer_keep_rearm": b"PICASSO TIMER: keep rearm end" in text,
        "timer_defer_ok": (
            b"PICASSO TIMER: defer post" in text and b"ok=1" in text
        ),
        "timer_irq_first": timer_poll_irqs[0] if timer_poll_irqs else None,
        "timer_irq_last": timer_poll_irqs[-1] if timer_poll_irqs else None,
        "timer_irq_growing": (
            len(timer_poll_irqs) >= 2
            and timer_poll_irqs[-1] > timer_poll_irqs[0]
        ),
        "control_window_complete": (
            b"PICASSO USB: control window complete" in text
        ),
        "shared_region_map_success": (
            b"PICASSO SR: map exit pid=1 error=0 kr=0x0" in text
        ),
        "early_boot_complete": (
            b"Early boot complete. Continuing system boot." in text
        ),
        "services_seen": sorted(
            name
            for name in (
                "SpringBoard",
                "backboardd",
                "runningboardd",
                "powerd",
            )
            if name.encode() in text
        ),
        "panic_marker_seen": b"=== PICASSO CDC PANIC BEGIN ===" in text,
        "reboot_fastboot_sent": reboot_sent,
        "reboot_fastboot_attempts": reboot_attempts,
        "reboot_fastboot_acknowledged": (
            b"PICASSO CDC: reboot-fastboot requested" in text
        ),
    }
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    event(f"RESULT {json.dumps(result)}")

    passed = all(
        (
            result["status_response"],
            result["help_response"],
            result["timer_snapshot_complete"],
            result["timer_sleep_passes"] == 10,
            result["timer_sleep_summary_pass"],
            result["timer_selftest_end_kept"],
            result["timer_selftest_done"],
            result["timer_keep_rearm"],
            result["timer_defer_ok"],
            result["timer_irq_growing"],
            result["control_window_complete"],
            result["shared_region_map_success"],
            result["early_boot_complete"],
            len(result["services_seen"]) == 4,
            not result["panic_marker_seen"],
            result["reboot_fastboot_sent"],
            result["reboot_fastboot_acknowledged"],
            result["returned_real_fastboot"],
        )
    )
    return 0 if passed else 2


if __name__ == "__main__":
    sys.exit(main())
