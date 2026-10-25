#!/usr/bin/env python3
"""Run one Picasso boot test over USB and archive the evidence.

The script deliberately has no write path: it only uses Fastboot getvar and
`fastboot boot`. It starts the CDC console capture before the boot, waits for
the selected device to leave and return to Fastboot, then stops capture and
records the image hash, command output, serial log, and a machine-readable
summary.
"""

import argparse
from datetime import datetime, timezone
import glob
import hashlib
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
CAPTURE_SCRIPT = ROOT / 'scripts/capture-picasso-usb-console.py'


class TestFailure(RuntimeError):
    pass


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec='seconds')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def serial_ports():
    return sorted(glob.glob('/dev/cu.usbmodem*'))


def run_command(command, timeout):
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError as error:
        raise TestFailure(f'command not found: {command[0]}') from error
    except subprocess.TimeoutExpired as error:
        stdout = (error.stdout or '') if isinstance(error.stdout, str) else ''
        stderr = (error.stderr or '') if isinstance(error.stderr, str) else ''
        raise TestFailure(
            f'timed out after {timeout:.0f}s: {" ".join(command)}'
            f'\nstdout:\n{stdout}\nstderr:\n{stderr}'
        ) from error
    return result


def combined_output(result):
    if result.stdout and result.stderr:
        return f'{result.stdout.rstrip()}\n{result.stderr.rstrip()}\n'
    return result.stdout or result.stderr or ''


def fastboot_devices(fastboot, timeout):
    result = run_command([fastboot, 'devices', '-l'], timeout)
    if result.returncode:
        raise TestFailure(f'fastboot devices failed:\n{combined_output(result)}')
    devices = {}
    for line in combined_output(result).splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] == 'fastboot':
            devices[fields[0]] = line
    return devices


def fastboot_getvar(fastboot, serial, name, timeout):
    result = run_command([fastboot, '-s', serial, 'getvar', name], timeout)
    output = combined_output(result)
    match = re.search(rf'^{re.escape(name)}:\s*(.*?)\s*$', output, re.MULTILINE)
    if result.returncode or match is None:
        raise TestFailure(f'fastboot getvar {name} failed:\n{output}')
    return match.group(1), output


def choose_serial(requested, devices):
    if requested:
        if requested not in devices:
            found = ', '.join(sorted(devices)) or 'none'
            raise TestFailure(
                f'Fastboot serial {requested} is not present; connected Fastboot devices: {found}'
            )
        return requested
    if not devices:
        raise TestFailure('No Fastboot device is connected')
    if len(devices) != 1:
        found = ', '.join(sorted(devices))
        raise TestFailure(f'Multiple Fastboot devices are connected ({found}); pass --serial')
    return next(iter(devices))


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')


def stop_process(process, timeout=10):
    if process is None or process.poll() is not None:
        return process.returncode if process is not None else None
    process.send_signal(signal.SIGINT)
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            return process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            return process.wait(timeout=3)


def write_checksums(output_dir):
    sums = output_dir / 'SHA256SUMS'
    lines = []
    for path in sorted(output_dir.rglob('*')):
        if path.is_file() and path != sums:
            lines.append(f'{sha256(path)}  {path.relative_to(output_dir)}')
    write_text(sums, '\n'.join(lines) + '\n')


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--boot-image', type=Path, required=True,
                        help='boot image passed to fastboot boot')
    parser.add_argument('--serial', help='Fastboot serial; inferred when exactly one device is present')
    parser.add_argument('--expect-sha256',
                        help='refuse to boot unless the image has this SHA-256')
    parser.add_argument('--output', type=Path,
                        help='evidence directory (defaults under out/picasso/logs/fastboot-runs)')
    parser.add_argument('--fastboot', default='fastboot', help='Fastboot executable')
    parser.add_argument('--command-timeout', type=float, default=120,
                        help='timeout for each Fastboot command')
    parser.add_argument('--return-timeout', type=float, default=300,
                        help='seconds to wait for the device to return to Fastboot')
    parser.add_argument('--capture-timeout', type=float, default=300,
                        help='seconds the CDC capture remains active')
    parser.add_argument('--drain-seconds', type=float, default=4,
                        help='capture drain time after Fastboot returns')
    parser.add_argument('--allow-no-cdc', action='store_true',
                        help='pass if the device returns to Fastboot without a CDC console')
    parser.add_argument('--require-log-regex', action='append', default=[],
                        help='fail unless this regular expression occurs in the CDC log; repeatable')
    args = parser.parse_args()
    if args.command_timeout <= 0 or args.return_timeout <= 0 or args.capture_timeout <= 0:
        parser.error('timeouts must be greater than zero')
    if args.drain_seconds < 0:
        parser.error('--drain-seconds cannot be negative')
    if args.allow_no_cdc and args.require_log_regex:
        parser.error('--allow-no-cdc cannot be combined with --require-log-regex')
    for expression in args.require_log_regex:
        try:
            re.compile(expression)
        except re.error as error:
            parser.error(f'invalid --require-log-regex {expression!r}: {error}')
    return args


def main():
    args = parse_args()
    image = args.boot_image.expanduser().resolve()
    if not image.is_file():
        print(f'error: boot image does not exist: {image}', file=sys.stderr)
        return 2

    actual_sha256 = sha256(image)
    if args.expect_sha256 and actual_sha256.lower() != args.expect_sha256.lower():
        print(
            f'error: boot image SHA-256 mismatch\nexpected: {args.expect_sha256.lower()}\n'
            f'actual:   {actual_sha256}',
            file=sys.stderr,
        )
        return 2
    with image.open('rb') as source:
        if source.read(8) != b'ANDROID!':
            print(f'error: {image} does not start with the Android boot-image magic', file=sys.stderr)
            return 2

    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    output_dir = (
        args.output.expanduser().resolve()
        if args.output
        else ROOT / 'out/picasso/logs/fastboot-runs' / stamp
    )
    if output_dir.exists() and any(output_dir.iterdir()):
        print(f'error: evidence directory is not empty: {output_dir}', file=sys.stderr)
        return 2
    (output_dir / 'logs').mkdir(parents=True, exist_ok=True)
    (output_dir / 'boot').mkdir(parents=True, exist_ok=True)

    archived_image = output_dir / 'boot' / image.name
    with image.open('rb') as source, archived_image.open('wb') as destination:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            destination.write(chunk)

    capture_log = output_dir / 'logs/kernel-console.log'
    capture_driver_log = output_dir / 'logs/capture-driver.log'
    fastboot_before_log = output_dir / 'fastboot-before.log'
    fastboot_boot_log = output_dir / 'fastboot-boot.log'
    fastboot_after_log = output_dir / 'fastboot-after.log'
    metadata_path = output_dir / 'metadata.json'

    metadata = {
        'started_utc': utc_now(),
        'finished_utc': None,
        'write_operations': False,
        'boot_method': 'fastboot boot',
        'boot_image': {
            'source': str(image),
            'archived': str(archived_image.relative_to(output_dir)),
            'sha256': actual_sha256,
            'expected_sha256': args.expect_sha256.lower() if args.expect_sha256 else None,
        },
        'fastboot': {
            'binary': args.fastboot,
            'serial': None,
            'before': None,
            'returned': False,
            'after': None,
        },
        'capture': {
            'ports_seen': [],
            'log': str(capture_log.relative_to(output_dir)),
            'bytes': 0,
            'allow_no_cdc': args.allow_no_cdc,
            'required_patterns': args.require_log_regex,
            'patterns_found': {},
        },
        'boot_command': {
            'command': None,
            'returncode': None,
            'stdout_stderr': None,
        },
        'outcome': 'not_started',
        'reason': None,
    }

    capture_process = None
    capture_handle = None
    failure = None
    try:
        devices = fastboot_devices(args.fastboot, args.command_timeout)
        serial = choose_serial(args.serial, devices)
        metadata['fastboot']['serial'] = serial
        before_line = devices[serial]
        metadata['fastboot']['before'] = before_line
        write_text(fastboot_before_log, before_line + '\n')

        product, product_output = fastboot_getvar(
            args.fastboot, serial, 'product', args.command_timeout
        )
        write_text(output_dir / 'fastboot-product.log', product_output)
        if product != 'picasso':
            raise TestFailure(f'connected Fastboot product is {product!r}, not "picasso"')

        userspace, userspace_output = fastboot_getvar(
            args.fastboot, serial, 'is-userspace', args.command_timeout
        )
        write_text(output_dir / 'fastboot-is-userspace.log', userspace_output)
        if userspace not in ('no', 'false', '0'):
            raise TestFailure(
                f'device reports is-userspace={userspace!r}; bootloader Fastboot is required'
            )

        existing_ports = serial_ports()
        if existing_ports:
            raise TestFailure(
                'USB modem port(s) are already present before boot: '
                f'{", ".join(existing_ports)}. Replug the phone and re-enter Fastboot '
                'so an old CDC session cannot be mistaken for this run.'
            )

        capture_command = [
            sys.executable,
            str(CAPTURE_SCRIPT),
            '--wait', str(args.capture_timeout),
            '--duration', str(args.capture_timeout),
            '--reconnect',
            '--no-stdout',
            '--output', str(capture_log),
        ]
        capture_handle = capture_driver_log.open('w', encoding='utf-8')
        capture_process = subprocess.Popen(
            capture_command,
            stdout=capture_handle,
            stderr=subprocess.STDOUT,
            text=True,
        )

        boot_command = [args.fastboot, '-s', serial, 'boot', str(image)]
        metadata['boot_command']['command'] = boot_command
        boot_result = run_command(boot_command, args.command_timeout)
        boot_output = combined_output(boot_result)
        metadata['boot_command']['returncode'] = boot_result.returncode
        metadata['boot_command']['stdout_stderr'] = boot_output
        write_text(fastboot_boot_log, boot_output)
        if boot_result.returncode:
            raise TestFailure(f'fastboot boot failed:\n{boot_output}')

        deadline = time.monotonic() + args.return_timeout
        saw_fastboot_offline = False
        ports_seen = set()
        while time.monotonic() < deadline:
            ports_seen.update(serial_ports())
            current_devices = fastboot_devices(args.fastboot, args.command_timeout)
            if serial not in current_devices:
                saw_fastboot_offline = True
            elif saw_fastboot_offline:
                metadata['fastboot']['returned'] = True
                metadata['fastboot']['after'] = current_devices[serial]
                write_text(fastboot_after_log, current_devices[serial] + '\n')
                break
            if capture_process.poll() is not None and not ports_seen and not args.allow_no_cdc:
                capture_driver_log_text = capture_driver_log.read_text(
                    encoding='utf-8', errors='replace'
                )
                raise TestFailure(
                    'CDC capture exited before a USB modem appeared:\n'
                    f'{capture_driver_log_text}'
                )
            time.sleep(0.5)

        metadata['capture']['ports_seen'] = sorted(ports_seen)
        if not metadata['fastboot']['returned']:
            raise TestFailure(
                f'device did not return to Fastboot within {args.return_timeout:.0f}s'
            )
        if not ports_seen and not args.allow_no_cdc:
            raise TestFailure('no CDC USB modem appeared during the boot')

        drain_deadline = time.monotonic() + args.drain_seconds
        while time.monotonic() < drain_deadline:
            ports_seen.update(serial_ports())
            time.sleep(0.1)
    except TestFailure as error:
        failure = str(error)
    except KeyboardInterrupt:
        failure = 'interrupted by user'
    finally:
        capture_returncode = stop_process(capture_process)
        if capture_handle is not None:
            capture_handle.close()
        metadata['capture']['returncode'] = capture_returncode

    log_bytes = capture_log.stat().st_size if capture_log.exists() else 0
    metadata['capture']['bytes'] = log_bytes
    if capture_log.exists():
        log_text = capture_log.read_text(encoding='utf-8', errors='replace')
        for expression in args.require_log_regex:
            metadata['capture']['patterns_found'][expression] = bool(
                re.search(expression, log_text, re.MULTILINE)
            )

    if failure:
        metadata['outcome'] = 'failed'
        metadata['reason'] = failure
        exit_code = 1
    elif (metadata['fastboot']['returned']
          and all(metadata['capture']['patterns_found'].values())
          and capture_returncode in (0, -signal.SIGINT, None)
          and (log_bytes > 0 or args.allow_no_cdc)):
        metadata['outcome'] = 'passed'
        metadata['reason'] = ('device returned to Fastboot automatically'
                              + ('; no CDC console' if log_bytes == 0 else ' after CDC capture'))
        exit_code = 0
    elif log_bytes == 0:
        metadata['outcome'] = 'failed'
        metadata['reason'] = 'CDC port appeared, but no kernel log bytes were captured'
        exit_code = 1
    elif capture_returncode not in (0, -signal.SIGINT):
        metadata['outcome'] = 'failed'
        metadata['reason'] = f'CDC capture exited with status {capture_returncode}'
        exit_code = 1
    else:
        missing = [
            expression for expression, found
            in metadata['capture']['patterns_found'].items() if not found
        ]
        metadata['outcome'] = 'failed'
        metadata['reason'] = f'required CDC log pattern(s) not found: {missing}'
        exit_code = 1

    metadata['finished_utc'] = utc_now()
    write_text(metadata_path, json.dumps(metadata, indent=2) + '\n')
    write_checksums(output_dir)

    print(f'Evidence: {output_dir}')
    print(f'CDC log:  {capture_log} ({log_bytes} bytes)')
    print(f'Outcome:  {metadata["outcome"]} - {metadata["reason"]}')
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
