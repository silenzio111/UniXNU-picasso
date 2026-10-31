#!/usr/bin/env python3
"""Send a command to the Picasso CDC-ACM console and capture its output."""
import argparse
from datetime import datetime
import fcntl
import glob
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import termios
import time


COMMANDS = ('help', 'status', 'panic-test', 'reset-test', 'start-graphics',
            'register-display', 'publish-display', 'publish-display-start',
            'hang-test', 'reboot-fastboot')


def serial_ports():
    return sorted(glob.glob('/dev/cu.usbmodem*'))


def configure_serial(fd):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def modem_bits(fd):
    return struct.unpack('I', fcntl.ioctl(
        fd, termios.TIOCMGET, struct.pack('I', 0)))[0]


def assert_modem_lines(fd):
    wanted = termios.TIOCM_DTR | termios.TIOCM_RTS
    before = modem_bits(fd)
    fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', wanted))
    after = modem_bits(fd)
    if after & wanted != wanted:
        raise OSError(
            f'failed to assert DTR/RTS: TIOCM before={before:#x} '
            f'after={after:#x}, wanted={wanted:#x}'
        )
    return before, after


def wait_for_fastboot(timeout, expected_serial):
    deadline = time.monotonic() + timeout
    absent_since = None
    while time.monotonic() < deadline:
        try:
            result = subprocess.run(
                ['fastboot', 'devices', '-l'],
                check=False,
                capture_output=True,
                text=True,
                timeout=3,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f'fastboot polling failed: {error}', file=sys.stderr)
            return False
        if result.returncode:
            print(
                f'fastboot devices failed with status {result.returncode}: '
                f'{result.stderr.strip()}',
                file=sys.stderr,
            )
            return False
        devices = {}
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[1] == 'fastboot':
                devices[fields[0]] = line
        now = time.monotonic()
        if expected_serial not in devices:
            absent_since = now
        elif absent_since is not None and now - absent_since >= 1:
            print(devices[expected_serial], file=sys.stderr)
            return True
        time.sleep(0.25)
    if expected_serial in devices:
        print(
            f'Fastboot serial {expected_serial} never disappeared; '
            'a stale pre-reset enumeration cannot prove the reboot succeeded',
            file=sys.stderr,
        )
    return False


def write_all(fd, data, timeout):
    deadline = time.monotonic() + timeout
    view = memoryview(data)
    total = 0
    while view:
        try:
            written = os.write(fd, view)
        except BlockingIOError:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f'serial write timed out after {timeout:.3f}s '
                    f'({total}/{len(data)} bytes accepted)'
                )
            select.select([], [fd], [], min(0.25, remaining))
            continue
        if written <= 0:
            raise OSError('serial write returned zero bytes')
        total += written
        view = view[written:]
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f'serial write timed out after {timeout:.3f}s '
                f'({total}/{len(data)} bytes accepted)'
            )
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='serial device, e.g. /dev/cu.usbmodem1101')
    parser.add_argument('--command', required=True, choices=COMMANDS)
    parser.add_argument('--output', type=Path,
                        help='log file (defaults to out/picasso/logs)')
    parser.add_argument('--wait', type=float, default=15,
                        help='seconds to wait for the USB modem')
    parser.add_argument('--duration', type=float, default=15,
                        help='seconds to keep capturing after sending the command')
    parser.add_argument('--repeat', type=int, default=1,
                        help='number of times to send the command')
    parser.add_argument('--repeat-interval', type=float, default=1.0,
                        help='seconds between repeated command writes')
    parser.add_argument('--write-timeout', type=float, default=5.0,
                        help='maximum seconds for each complete command write')
    parser.add_argument('--no-stdout', action='store_true',
                        help='write only to the log file')
    parser.add_argument('--fastboot-timeout', type=float,
                        help='after USB disconnects, wait this long for Fastboot')
    parser.add_argument('--fastboot-serial',
                        help='serial that must re-enumerate in Fastboot')
    args = parser.parse_args()

    if args.wait < 0:
        parser.error('--wait must be zero or greater')
    if args.duration <= 0:
        parser.error('--duration must be greater than zero')
    if args.repeat <= 0:
        parser.error('--repeat must be greater than zero')
    if args.repeat_interval <= 0:
        parser.error('--repeat-interval must be greater than zero')
    if args.write_timeout <= 0:
        parser.error('--write-timeout must be greater than zero')
    if args.fastboot_timeout is not None and args.fastboot_timeout <= 0:
        parser.error('--fastboot-timeout must be greater than zero')
    if (args.fastboot_timeout is None) != (args.fastboot_serial is None):
        parser.error(
            '--fastboot-timeout and --fastboot-serial must be provided together'
        )

    port_deadline = time.monotonic() + args.wait
    ports = serial_ports()
    while True:
        if args.port:
            port = args.port if os.path.exists(args.port) else None
        elif len(ports) == 1:
            port = ports[0]
        elif len(ports) > 1:
            parser.error('Multiple USB modems found; choose one with --port')
        else:
            port = None
        if port is not None:
            break
        if time.monotonic() >= port_deadline:
            parser.error('No USB modem appeared within the wait period')
        time.sleep(0.1)
        ports = serial_ports()

    script_root = Path(__file__).resolve().parents[1]
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    output = args.output or (
        script_root / 'out/picasso/logs' /
        f'picasso-cdc-{args.command.replace("/", "-")}-{stamp}.log'
    )
    output.parent.mkdir(parents=True, exist_ok=True)

    fd = None
    disconnected = False
    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure_serial(fd)
        bits_before, bits_after = assert_modem_lines(fd)
        payload = (args.command + '\r\n').encode('ascii')
        write_started = time.monotonic()
        written = write_all(fd, payload, args.write_timeout)
        write_elapsed = time.monotonic() - write_started
        # tcdrain() can block indefinitely on the DWC3-backed CDC interface
        # while the device is in its early boot poll loop. The write has
        # already been accepted by the tty layer; the command parser will see
        # the bytes once the device polls EP2.
        drain_elapsed = 0.0
        sent = 1
        next_send = time.monotonic() + args.repeat_interval
        print(
            f'Sent {args.command!r} to {port}: wrote={written}/{len(payload)} '
            f'TIOCM={bits_before:#x}->{bits_after:#x} '
            f'write={write_elapsed:.6f}s tcdrain={drain_elapsed:.6f}s; '
            f'recording to {output}',
            file=sys.stderr,
        )

        deadline = time.monotonic() + args.duration
        with output.open('ab') as log:
            while time.monotonic() < deadline:
                if sent < args.repeat and time.monotonic() >= next_send:
                    written = write_all(fd, payload, args.write_timeout)
                    print(f'Resent {args.command!r}: wrote={written}/{len(payload)}',
                          file=sys.stderr)
                    sent += 1
                    next_send = time.monotonic() + args.repeat_interval
                timeout = min(0.5, max(0, deadline - time.monotonic()))
                try:
                    ready, _, _ = select.select([fd], [], [], timeout)
                except (OSError, ValueError):
                    disconnected = True
                    break
                if not ready:
                    continue
                try:
                    data = os.read(fd, 65536)
                except BlockingIOError:
                    continue
                except OSError:
                    disconnected = True
                    break
                if not data:
                    disconnected = True
                    break
                log.write(data)
                log.flush()
                if not args.no_stdout:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
    except OSError as error:
        disconnected = True
        print(f'USB serial connection ended: {error}', file=sys.stderr)
    finally:
        if fd is not None:
            os.close(fd)

    print(f'Log saved to {output}', file=sys.stderr)
    if args.fastboot_timeout is not None:
        if not disconnected:
            print('USB did not disconnect; Fastboot was not polled', file=sys.stderr)
            return 1
        if wait_for_fastboot(args.fastboot_timeout, args.fastboot_serial):
            print('Device is back in Fastboot', file=sys.stderr)
            return 0
        print('Device did not return to Fastboot within the timeout', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
