#!/usr/bin/env python3
"""
dump-picasso-panic.py - Host-side Panic Log & dmesg Extractor for Redmi K30 5G (SM7250 / picasso)

Extracts Darwin 23 / iOS 17.7.11 panic logs and kernel console buffers across warm reboots
over USB without physical hardware teardown.

Supported input methods:
  1. Local memory dump / file (--file <path>)
  2. USB Fastboot memory extraction (--fastboot)
  3. USB ADB memory / sysfs reading (--adb)
  4. USB EDL (9008) Qualcomm memory dump (--edl)
  5. FAT32 Payload / EFI image inspection (--payload <image>)
"""

import argparse
import binascii
import os
import pathlib
import re
import struct
import subprocess
import sys
from typing import Optional, Tuple

EMBEDDED_PANIC_MAGIC = 0x46554E4B  # 'FUNK'
MACOS_PANIC_MAGIC    = 0x44454544  # 'DEED'

# Physical DRAM reservation on SM7250
DEFAULT_PRAM_PADDR = 0xB7F00000  # matches /pram reg in scripts/make-picasso-adt.py
DEFAULT_PRAM_SIZE  = 0x00100000  # 1 MiB

PANIC_FLAG_NAMES = {
    0x01:    "COREDUMP_COMPLETE",
    0x02:    "STACKSHOT_SUCCEEDED",
    0x04:    "STACKSHOT_FAILED_DEBUGGERSYNC",
    0x08:    "STACKSHOT_FAILED_ERROR",
    0x10:    "STACKSHOT_FAILED_INCOMPLETE",
    0x20:    "STACKSHOT_FAILED_NESTED",
    0x40:    "NESTED_PANIC",
    0x80:    "BUTTON_RESET_PANIC",
    0x100:   "COPROC_INITIATED_PANIC",
    0x200:   "COREDUMP_FAILED",
    0x400:   "COMPRESS_FAILED",
    0x800:   "STACKSHOT_DATA_COMPRESSED",
    0x1000:  "ENCRYPTED_COREDUMP_SKIPPED",
    0x2000:  "KERNEL_COREDUMP_SKIPPED_EXCLUDE_REGIONS_UNAVAILABLE",
    0x4000:  "COREFILE_UNLINKED",
    0x8000:  "INCOHERENT_PANICLOG",
    0x10000: "EXCLAVE_PANIC",
}


def decode_flags(flags: int) -> list:
    active = []
    for bit, name in PANIC_FLAG_NAMES.items():
        if flags & bit:
            active.append(name)
    return active


def parse_embedded_panic_header(data: bytes, offset: int = 0) -> Optional[dict]:
    """Parse struct embedded_panic_header at given offset."""
    if len(data) < offset + 48:
        return None

    magic = struct.unpack_from("<I", data, offset)[0]
    if magic != EMBEDDED_PANIC_MAGIC:
        return None

    # Header layout:
    #   uint32_t eph_magic
    #   uint32_t eph_crc
    #   uint32_t eph_version
    #   uint64_t eph_panic_flags
    #   uint32_t eph_panic_log_offset
    #   uint32_t eph_panic_log_len
    #   uint32_t eph_stackshot_offset
    #   uint32_t eph_stackshot_len
    #   uint32_t eph_other_log_offset
    #   uint32_t eph_other_log_len
    #   uint64_t eph_x86_do_not_use
    #   char eph_os_version[32]
    #   char eph_macos_version[32]
    #   uuid_string_t eph_bootsessionuuid_string (37 bytes)
    #   uint64_t eph_roots_installed
    #   uint32_t eph_ext_paniclog_offset
    #   uint32_t eph_ext_paniclog_len
    try:
        (
            eph_magic,
            eph_crc,
            eph_version,
            eph_panic_flags,
            eph_panic_log_offset,
            eph_panic_log_len,
            eph_stackshot_offset,
            eph_stackshot_len,
            eph_other_log_offset,
            eph_other_log_len,
            eph_x86,
        ) = struct.unpack_from("<IIIQIIIIIIQ", data, offset)

        str_base = offset + struct.calcsize("<IIIQIIIIIIQ")
        os_version = ""
        macos_version = ""
        boot_uuid = ""

        if len(data) >= str_base + 32:
            os_version = data[str_base : str_base + 32].split(b"\x00")[0].decode("ascii", errors="replace")
        if len(data) >= str_base + 64:
            macos_version = data[str_base + 32 : str_base + 64].split(b"\x00")[0].decode("ascii", errors="replace")
        if len(data) >= str_base + 64 + 37:
            boot_uuid = data[str_base + 64 : str_base + 64 + 37].split(b"\x00")[0].decode("ascii", errors="replace")

        panic_text = ""
        if eph_panic_log_len > 0 and (offset + eph_panic_log_offset + eph_panic_log_len) <= len(data):
            p_bytes = data[offset + eph_panic_log_offset : offset + eph_panic_log_offset + eph_panic_log_len]
            panic_text = p_bytes.decode("utf-8", errors="replace")

        other_text = ""
        if eph_other_log_len > 0 and (offset + eph_other_log_offset + eph_other_log_len) <= len(data):
            o_bytes = data[offset + eph_other_log_offset : offset + eph_other_log_offset + eph_other_log_len]
            other_text = o_bytes.decode("utf-8", errors="replace")

        stackshot_data = None
        if eph_stackshot_len > 0 and (offset + eph_stackshot_offset + eph_stackshot_len) <= len(data):
            stackshot_data = data[offset + eph_stackshot_offset : offset + eph_stackshot_offset + eph_stackshot_len]

        return {
            "type": "embedded",
            "offset": offset,
            "magic": hex(eph_magic),
            "crc": hex(eph_crc),
            "version": eph_version,
            "flags": eph_panic_flags,
            "flags_decoded": decode_flags(eph_panic_flags),
            "panic_log_offset": eph_panic_log_offset,
            "panic_log_len": eph_panic_log_len,
            "stackshot_offset": eph_stackshot_offset,
            "stackshot_len": eph_stackshot_len,
            "other_log_offset": eph_other_log_offset,
            "other_log_len": eph_other_log_len,
            "os_version": os_version,
            "macos_version": macos_version,
            "boot_uuid": boot_uuid,
            "panic_text": panic_text,
            "other_text": other_text,
            "has_stackshot": stackshot_data is not None,
            "stackshot_data": stackshot_data,
        }
    except Exception as err:
        return None


def search_panic_signatures(data: bytes) -> list:
    """Scan data for FUNK magic or raw Darwin panic strings."""
    results = []
    seen_offsets = set()

    # 1. Search EMBEDDED_PANIC_MAGIC in Little-Endian (b'KNUF') and Big-Endian (b'FUNK')
    magic_candidates = [
        struct.pack("<I", EMBEDDED_PANIC_MAGIC),
        struct.pack(">I", EMBEDDED_PANIC_MAGIC),
    ]

    for magic_bytes in magic_candidates:
        pos = 0
        while True:
            pos = data.find(magic_bytes, pos)
            if pos == -1:
                break
            if pos not in seen_offsets:
                hdr = parse_embedded_panic_header(data, pos)
                if hdr and (hdr["panic_log_len"] > 0 or hdr["version"] in (4, 5)):
                    results.append(hdr)
                    seen_offsets.add(pos)
            pos += 4

    # 2. Search raw Darwin panic strings if no structured header found
    if not results:
        raw_patterns = [
            b"panic(cpu",
            b"Debugger called: <panic>",
            b"Darwin Kernel Version",
            b"Kernel Extensions in backtrace:",
            b"macOS Panic String:",
        ]
        found_spans = []
        for pat in raw_patterns:
            p = 0
            while True:
                p = data.find(pat, p)
                if p == -1:
                    break
                block_start = max(0, p - 64)
                found_spans.append(block_start)
                p += len(pat)

        # Merge overlapping spans within 2048 bytes
        found_spans = sorted(set(found_spans))
        merged_spans = []
        for s in found_spans:
            if not merged_spans or s > (merged_spans[-1] + 2048):
                merged_spans.append(s)

        for off in merged_spans:
            chunk = data[off : off + 32768]
            match = re.search(b"[\\x20-\\x7E\\r\\n\\t]{40,}", chunk)
            if match:
                text = match.group(0).decode("utf-8", errors="replace")
                results.append({
                    "type": "raw_carved",
                    "offset": off + match.start(),
                    "panic_text": text,
                    "other_text": "",
                    "flags_decoded": ["RAW_TEXT_RECOVERED"],
                })

    return results


def run_cmd(cmd: list) -> Tuple[int, str, str]:
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        return res.returncode, res.stdout.decode("utf-8", errors="replace"), res.stderr.decode("utf-8", errors="replace")
    except Exception as e:
        return -1, "", str(e)



def run_cmd_bytes(cmd: list) -> Tuple[int, bytes, str]:
    """Run a command and return stdout as raw bytes (binary-safe, for memory dumps)."""
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        return res.returncode, res.stdout, res.stderr.decode("utf-8", errors="replace")
    except Exception as e:
        return -1, b"", str(e)

def extract_fastboot_ramdump(out_path: str, paddr: int, size: int) -> bool:
    """Attempt USB fastboot memory dump via OEM extension commands."""
    print("Checking USB fastboot device...")
    rc, stdout, stderr = run_cmd(["fastboot", "devices"])
    if rc != 0 or not stdout.strip():
        print("No fastboot device found over USB. Ensure device is in fastboot mode ('adb reboot bootloader').")
        return False

    dev_line = stdout.strip().split("\n")[0]
    print(f"Connected fastboot device: {dev_line}")

    # Qualcomm fastboot OEM ramdump commands
    trial_cmds = [
        ["fastboot", "oem", "dump", hex(paddr), hex(size)],
        ["fastboot", "oem", "ramdump"],
        ["fastboot", "fetch", "pram", out_path],
    ]

    for c in trial_cmds:
        print(f"Executing: {' '.join(c)}...")
        rc, out, err = run_cmd(c)
        if rc == 0 and os.path.exists(out_path) and os.path.getsize(out_path) > 0:
            print(f"Successfully dumped to {out_path} ({os.path.getsize(out_path)} bytes)")
            return True

    print("Fastboot OEM dump commands not supported by current aboot / bootloader.")
    return False


def extract_adb_ramdump(out_path: str, paddr: int, size: int) -> bool:
    """Extract memory buffer via ADB when booted into TWRP or Linux recovery."""
    print("Checking USB ADB device...")
    rc, stdout, stderr = run_cmd(["adb", "devices"])
    if rc != 0 or "device" not in stdout and "recovery" not in stdout:
        print("No authorized ADB device found over USB.")
        return False

    print("Connected ADB device detected. Attempting extraction...")
    # Attempt 1: devmem / dev/mem
    skip_mb = paddr // (1024 * 1024)
    count_mb = max(1, size // (1024 * 1024))
    dd_cmd = f"dd if=/dev/mem bs=1M skip={skip_mb} count={count_mb} 2>/dev/null"
    rc, out, err = run_cmd_bytes(["adb", "exec-out", f"su -c '{dd_cmd}' || {dd_cmd}"])
    if rc == 0 and len(out) > 1024:
        with open(out_path, "wb") as f:
            f.write(out)
        print(f"Extracted {os.path.getsize(out_path)} bytes via /dev/mem.")
        return True

    # Attempt 2: /sys/fs/pstore or /proc/last_kmsg
    for kmsg_path in ["/sys/fs/pstore/console-ramoops", "/proc/last_kmsg", "/dev/pmsg0"]:
        rc, out, err = run_cmd(["adb", "shell", f"cat {kmsg_path} 2>/dev/null"])
        if rc == 0 and len(out.strip()) > 10:
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(out)
            print(f"Extracted panic log from {kmsg_path}.")
            return True

    return False


def extract_edl_ramdump(out_path: str, paddr: int, size: int) -> bool:
    """Extract memory buffer via Qualcomm EDL mode using edl tool."""
    print("Checking Qualcomm EDL (9008) USB device...")
    edl_cmd = ["edl", "memory", "dump", hex(paddr), hex(size), out_path]
    rc, out, err = run_cmd(edl_cmd)
    if rc == 0 and os.path.exists(out_path):
        print(f"Extracted {os.path.getsize(out_path)} bytes via EDL.")
        return True
    return False


def print_panic_report(report: dict, verbose: bool = False):
    print("=" * 80)
    print("DARWIN EMBEDDED PANIC REPORT (SM7250 / picasso)")
    print("=" * 80)
    print(f"Buffer Offset   : 0x{report.get('offset', 0):X}")
    if report.get("type") == "embedded":
        print(f"Magic           : {report.get('magic')} (Valid EMBEDDED_PANIC_MAGIC)")
        print(f"Header Version  : {report.get('version')}")
        print(f"Header Flags    : {hex(report.get('flags', 0))} -> {', '.join(report.get('flags_decoded', []))}")
        print(f"OS Version      : {report.get('os_version', 'Unknown')}")
        print(f"macOS Version   : {report.get('macos_version', 'Unknown')}")
        print(f"Boot Session ID : {report.get('boot_uuid', 'None')}")
        print(f"Panic Log Length: {report.get('panic_log_len')} bytes")
        print(f"Other Log Length: {report.get('other_log_len')} bytes")
        print(f"Stackshot Data  : {'Present' if report.get('has_stackshot') else 'None'}")
    else:
        print("Type            : Raw panic string match")
        print(f"Status          : {', '.join(report.get('flags_decoded', []))}")

    print("-" * 80)
    print("PANIC STRING & BACKTRACE:")
    print("-" * 80)
    panic_text = report.get("panic_text", "").strip()
    if panic_text:
        print(panic_text)
    else:
        print("(No panic text recorded)")

    other_text = report.get("other_text", "").strip()
    if other_text:
        print("-" * 80)
        print("KERNEL CONSOLE LOG (dmesg / kprintf):")
        print("-" * 80)
        print(other_text)

    print("=" * 80)




PICASSO_RING_PADDR = 0x100000000  # 4 GiB mark, above the 2 GiB the ADT declares; untouched by ABL/UEFI/XNU
PICASSO_RING_MAGIC  = 0x504F4C47  # 'PLOG'


def parse_picasso_ring(data: bytes) -> None:
    """Parse the picasso persistent console ring: 32-byte header + 1 MiB wrapping data."""
    if len(data) < 32 + 16:
        print("Ring extract too short.")
        return
    magic, version, generation, data_size, write_total = struct.unpack_from("<IIIIQ", data, 0)
    if magic != PICASSO_RING_MAGIC:
        print(f"No PLOG magic at ring base (found {magic:#x}).")
        return
    print(f"PLOG version={version} generation={generation} data_size={data_size} write_total={write_total}")
    buf = data[32:32 + data_size]
    if len(buf) < data_size:
        print("Truncated ring data.")
    have = min(write_total, data_size)
    tail = bytes(buf[(write_total - i) % data_size] for i in range(have, 0, -1))
    text = "".join(chr(b) if 32 <= b < 127 or b in (10, 13, 9) else "." for b in tail)
    out = pathlib.Path("picasso-ring.log")
    out.write_text(text)
    print(f"Wrote {len(text)} bytes to {out.resolve()}")
    print("---- last 2048 bytes ----")
    print(text[-2048:])

def main():
    parser = argparse.ArgumentParser(
        description="Extract and parse Darwin / iOS panic buffer across warm reboot on Redmi K30 5G (SM7250 / picasso)."
    )
    parser.add_argument("--file", "-f", help="Local binary dump file to inspect.")
    parser.add_argument("--fastboot", action="store_true", help="Attempt fastboot OEM memory dump over USB.")
    parser.add_argument("--adb", action="store_true", help="Extract memory/logs via ADB over USB.")
    parser.add_argument("--edl", action="store_true", help="Extract memory via Qualcomm EDL 9008 mode.")
    parser.add_argument("--paddr", type=lambda x: int(x, 0), default=DEFAULT_PRAM_PADDR, help=f"Physical address of pram buffer (default: 0x{DEFAULT_PRAM_PADDR:X}).")
    parser.add_argument("--size", type=lambda x: int(x, 0), default=DEFAULT_PRAM_SIZE, help=f"Size of pram buffer in bytes (default: 0x{DEFAULT_PRAM_SIZE:X}).")
    parser.add_argument("--output", "-o", default="picasso-panic.bin", help="Destination file for extracted binary buffer.")
    parser.add_argument("--save-stackshot", help="Path to write raw stackshot data if present.")
    parser.add_argument("--ring", action="store_true", help="Parse the picasso persistent console ring (PLOG) instead of the panic header.")
    parser.add_argument("--verbose", "-v", action="store_true", help="Enable verbose diagnostic output.")

    args = parser.parse_args()

    data = None
    source_desc = ""

    if args.ring:
        if not args.file:
            args.paddr = PICASSO_RING_PADDR
            args.size = 0x100020
            if extract_adb_ramdump(args.output, args.paddr, args.size):
                args.file = args.output
        if args.file:
            parse_picasso_ring(open(args.file, "rb").read())
        return

    if args.file:
        source_desc = f"File '{args.file}'"
        if not os.path.exists(args.file):
            print(f"Error: Specified file not found: {args.file}")
            sys.exit(1)
        with open(args.file, "rb") as f:
            data = f.read()
    elif args.fastboot:
        source_desc = "USB Fastboot"
        if extract_fastboot_ramdump(args.output, args.paddr, args.size):
            with open(args.output, "rb") as f:
                data = f.read()
    elif args.adb:
        source_desc = "USB ADB"
        if extract_adb_ramdump(args.output, args.paddr, args.size):
            with open(args.output, "rb") as f:
                data = f.read()
    elif args.edl:
        source_desc = "Qualcomm EDL (9008)"
        if extract_edl_ramdump(args.output, args.paddr, args.size):
            with open(args.output, "rb") as f:
                data = f.read()
    else:
        # Default behavior: check if default output or mock exists, or probe USB interfaces sequentially
        print("No specific source flag provided. Probing connected USB interfaces...")
        if extract_fastboot_ramdump(args.output, args.paddr, args.size):
            source_desc = "USB Fastboot"
            with open(args.output, "rb") as f:
                data = f.read()
        elif extract_adb_ramdump(args.output, args.paddr, args.size):
            source_desc = "USB ADB"
            with open(args.output, "rb") as f:
                data = f.read()
        elif os.path.exists(args.output):
            source_desc = f"Existing dump file '{args.output}'"
            with open(args.output, "rb") as f:
                data = f.read()

    if data is None:
        print("\nCould not acquire panic buffer data.")
        print("Usage:")
        print("  1. When phone is booted into recovery or TWRP:")
        print("     python3 dump-picasso-panic.py --adb")
        print("  2. When phone warm-reboots into fastboot mode:")
        print("     python3 dump-picasso-panic.py --fastboot")
        print("  3. From an existing memory dump file:")
        print("     python3 dump-picasso-panic.py --file <path-to-memory.bin>")
        sys.exit(1)

    print(f"Scanning {len(data)} bytes from {source_desc} for Darwin panic structures...")
    reports = search_panic_signatures(data)

    if not reports:
        print("No Darwin embedded panic headers (magic 'FUNK') or panic strings found.")
        print("Reasons may include:")
        print("  - The kernel did not panic (system is running normally or cleanly powered off).")
        print("  - Cold boot cut PMIC power, clearing DRAM self-refresh.")
        print("  - Buffer address does not match 0x8FE00000.")
        sys.exit(0)

    for i, r in enumerate(reports, 1):
        print(f"\n[Record #{i}]")
        print_panic_report(r, verbose=args.verbose)

        if args.save_stackshot and r.get("has_stackshot"):
            with open(args.save_stackshot, "wb") as sf:
                sf.write(r["stackshot_data"])
            print(f"Stackshot saved to: {args.save_stackshot}")


if __name__ == "__main__":
    main()
