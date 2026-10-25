#!/usr/bin/env python3
"""Capture the Picasso CDC-ACM kernel console on macOS."""
import argparse
from datetime import datetime
import glob
import os
from pathlib import Path
import select
import sys
import termios
import time


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='serial device, e.g. /dev/cu.usbmodem1101')
    parser.add_argument('--output', type=Path, help='log file (defaults to out/picasso/logs)')
    parser.add_argument('--duration', type=float, help='stop after this many seconds')
    parser.add_argument('--wait', type=float, default=0,
                        help='wait up to this many seconds for the serial port')
    parser.add_argument('--reconnect', action='store_true',
                        help='reopen the port if the device disappears and enumerates again')
    parser.add_argument('--list', action='store_true', help='list matching serial devices')
    parser.add_argument('--no-stdout', action='store_true', help='write only to the log file')
    args = parser.parse_args()

    ports = serial_ports()
    if args.list:
        print('\n'.join(ports) if ports else 'No /dev/cu.usbmodem* devices found.')
        return 0
    if args.duration is not None and args.duration <= 0:
        parser.error('--duration must be greater than zero')
    if args.wait < 0:
        parser.error('--wait must be zero or greater')
    deadline = time.monotonic() + args.wait
    while not (os.path.exists(args.port) if args.port else ports):
        if time.monotonic() >= deadline:
            parser.error('No USB modem appeared within the wait period')
        time.sleep(min(0.1, max(0, deadline - time.monotonic())))
        ports = serial_ports()
    if args.port:
        port = args.port
    elif len(ports) == 1:
        port = ports[0]
    elif not ports:
        parser.error('No USB modem found; connect the device or pass --port explicitly')
    else:
        parser.error('Multiple USB modems found; choose one with --port')

    script_root = Path(__file__).resolve().parents[1]
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    output = args.output or script_root / 'out/picasso/logs' / f'picasso-usb-console-{stamp}.log'
    output.parent.mkdir(parents=True, exist_ok=True)

    deadline = time.monotonic() + args.duration if args.duration else None
    current_port = port
    try:
        with output.open('ab') as log:
            while deadline is None or time.monotonic() < deadline:
                if current_port is None:
                    wait_deadline = time.monotonic() + args.wait if log.tell() == 0 else None
                    while current_port is None:
                        ports = serial_ports()
                        if len(ports) == 1:
                            current_port = ports[0]
                        elif len(ports) > 1:
                            parser.error('Multiple USB modems found; choose one with --port')
                        now = time.monotonic()
                        if deadline is not None and now >= deadline:
                            break
                        if wait_deadline is not None and now >= wait_deadline:
                            parser.error('No USB modem appeared within the wait period')
                        time.sleep(0.1)
                    if current_port is None:
                        break

                fd = None
                try:
                    fd = os.open(current_port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
                    configure_serial(fd)
                    print(f'Recording {current_port} to {output} (Ctrl-C to stop)',
                          file=sys.stderr)
                    while deadline is None or time.monotonic() < deadline:
                        timeout = 0.5
                        if deadline is not None:
                            timeout = min(timeout, max(0, deadline - time.monotonic()))
                        try:
                            ready, _, _ = select.select([fd], [], [], timeout)
                        except (OSError, ValueError) as error:
                            if not args.reconnect:
                                raise error
                            ready = []
                        if not ready:
                            if args.reconnect and not os.path.exists(current_port):
                                break
                            continue
                        try:
                            data = os.read(fd, 65536)
                        except BlockingIOError:
                            continue
                        if not data:
                            continue
                        log.write(data)
                        log.flush()
                        if not args.no_stdout:
                            sys.stdout.buffer.write(data)
                            sys.stdout.buffer.flush()
                except OSError as error:
                    if not args.reconnect:
                        print(f'USB serial capture stopped: {error}', file=sys.stderr)
                        return 1
                finally:
                    if fd is not None:
                        os.close(fd)

                if not args.reconnect:
                    break
                print(f'USB serial device {current_port} disconnected; waiting for it to return',
                      file=sys.stderr)
                current_port = None
    except KeyboardInterrupt:
        pass
    print(f'Log saved to {output}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
