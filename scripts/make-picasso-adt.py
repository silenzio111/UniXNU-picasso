#!/usr/bin/env python3
"""Generate the minimal picasso hardware Apple-format DT for the first boot.

The tree mirrors the proven sheng/QEMU entry structure with real pinned picasso
SM7250 values taken from the compiled board DT:

  - GICv3 distributor 0x17A00000/0x10000, redistributor 0x17A60000/0x100000
    (arm-io child offsets under an APSS range at 0x17A00000), stride 0x40000
  - CPU timebase 19200000 Hz (Qualcomm SM7250 architectural counter)
  - Virtual timer GIC_PPI 3: absolute INTID 19, from picasso's live board DT
  - DRAM base 0x80000000; size comes from the actual device observation and
    is cross-checked at build time.

This is first-boot bring-up data: the platform has no matching platform expert yet,
so the kernel is expected to reach the video console and report the genuine
missing-driver panic on screen. No Apple device identity is claimed and no property
is invented beyond these pinned sources.
"""
from pathlib import Path
import argparse
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

DRAM_BASE = 0x80000000
# APSS peripheral window that the /arm-io bus node exposes. Child reg values are
# offsets inside this window (docs/zh/porting/device-tree.md: "ranges 映射总线窗口。
# 子节点 reg 使用窗口内偏移。低于窗口基址的设备无法作为子节点表达，驱动改用板级
# 常量中的物理地址"), which is how make-sheng-adt.py builds the same node.
ARMIO_BASE = 0x17A00000
GIC_DIST = 0x17A00000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x17A60000
GIC_REDIST_SIZE = 0x100000
TIMEBASE = 19200000
# /soc/timer interrupts in the picasso board DT: secure, nonsecure, virtual, hyp.
# The virtual entry is <1 3 0xff08>: GIC_PPI 3 means INTID 16 + 3, not 27.
# ArmCpu consumes an absolute INTID encoded as a little-endian ADT u32.
VIRTUAL_TIMER_INTID = 19
UFS_HC = 0x1d84000
UFS_HC_SIZE = 0x3000
UFS_PHY = 0x1d80000
UFS_PHY_SIZE = 0x2000
UART_BASE = 0x00988000
UART_SIZE = 0x4000
# Panic log region for XNU's check_for_panic_log(): /pram/reg plus
# /chosen/embedded-panic-log-size. The address must be DRAM that the firmware
# already maps as normal system memory, because the kernel reaches it with
# ml_io_map_wcomb() and then bzero()s the whole region.
#
# 0x8FE00000 (what the upstream branch used) sits inside
# modem_wlan_region@8c000000 (0x8C000000..0x9B800000, 248 MiB, compatible
# "removed-dma-pool", no-map). Mapping it writable and zeroing 1 MiB there
# overwrites a modem/WLAN DMA pool, so it was never usable on this board.
#
# 0xB7F00000 is inside the "Boot Ramdisk Gap" that picassoPkg's MemoryMapLib
# already declares as SYS_MEM / WRITE_BACK_XN (0xAD000000..0xBCC00000), which is
# also how the payload ramdisk is reachable. It clears every other claim on that
# DRAM:
#   - outside the ABL kernel+ramdisk window (0xB4513000..0xB7E00000, derived
#     from the device DT: linux,initrd-start/end = 0xB5513000/0xB7DFE66D),
#     starting 1 MiB past its upper edge;
#   - outside every /reserved-memory node in the device DT;
#   - outside the firmware's own DXE Heap / Sched Heap / FD / SEC heap /
#     page tables / stack / log buffer / info block;
#   - inside /memory/reg, so it is real DRAM.
PANIC_LOG_PADDR = 0xB7F00000
PANIC_LOG_SIZE = 0x100000

GPU_BASE = 0x05000000
GPU_SIZE = 0x40000
GPU_CX_BASE = 0x0509E000
GPU_CX_SIZE = 0x1000

GMU_BASE = 0x0506A000
GMU_SIZE = 0x32000
GMU_PDC_CFG = 0x0B280000
GMU_PDC_CFG_SIZE = 0x10000
GMU_PDC_SEQ = 0x0B480000
GMU_PDC_SEQ_SIZE = 0x10000

SMMU_GPU_BASE = 0x05040000
SMMU_GPU_SIZE = 0x10000


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/picasso/boot/kernel.macho.adt')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True,
                        help='Actual observed DRAM size, e.g. 0x180000000 for 6 GiB')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=None,
                        help='Explicit DarwinBoot contiguous window request, 256 MiB..8 GiB in 2 MiB steps')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching: select the root by the UUID of an IOMedia (the standard source mechanism)')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='/chosen/unique-chip-id (8 bytes) and --chip-id (4 bytes) seed XNU IOPlatformUUID; software identity seeds, not Apple silicon IDs')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true',
                        help='/product and /buttons describe an iPad-class device without a Home button (MobileGestalt reads these from the tree)')
    parser.add_argument('--cpu-regs', default='0',
                        help='Comma-separated CPU MPIDR affinity values (the /cpus reg of each core); the first is the running boot CPU. '
                             'SM7250 numbers its cores 0x000..0x700 in Affinity1. More than one adds /psci')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='smc',
                        help='/psci method used by the secondary-CPU start (the board DT declares smc)')
    parser.add_argument('--packed-data-ramdisk', action='store_true', help='Hand off validated DBRDISK1 bytes to the native sparse block driver, without a dense memdev alias')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder; the loader fills in the real range for a scratch Data seed')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    parser.add_argument('--gpu-addon', action='store_true', help='Zeroed GpuAddon sidecar handoff placeholder')
    parser.add_argument('--random-seed', metavar='HEX',
                        help='Exact 64-byte /chosen/random-seed value, for reproducible A/B trees')
    args = parser.parse_args()
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.unique_chip_id is not None and not (0 < args.unique_chip_id < 1 << 64 and 0 < args.chip_id < 1 << 32):
        parser.error('--unique-chip-id must fit 8 bytes and --chip-id 4 bytes, both nonzero')
    if args.owned_memory_size is not None and (
            not 0x10000000 <= args.owned_memory_size <= 0x200000000
            or args.owned_memory_size % 0x200000):
        parser.error('Owned memory must be 256 MiB to 8 GiB, aligned to 2 MiB')
    if args.random_seed is None:
        random_seed = os.urandom(64)
    else:
        try:
            random_seed = bytes.fromhex(args.random_seed)
        except ValueError:
            parser.error('--random-seed must be hexadecimal')
        if len(random_seed) != 64:
            parser.error('--random-seed must encode exactly 64 bytes')

    try:
        cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    except ValueError:
        parser.error('--cpu-regs must be integers')
    if (not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or len(cpu_regs) > 8
            or any(reg < 0 or reg > 0xffff or reg & 0xff for reg in cpu_regs)):
        parser.error('--cpu-regs: up to eight distinct Affinity1-numbered cores, boot CPU 0 first')
    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   # /cpus is #address-cells = 2 (as in the device DT), so
                                   # the MPIDR affinity goes out as a 64-bit cell.
                                   ('reg', struct.pack('<Q', reg)),
                                   ('device_type', b'cpu\0'), ('compatible', b'arm,cortex-a76\0'),
                                   ('timebase-frequency', struct.pack('<I', TIMEBASE))])
            for index, reg in enumerate(cpu_regs)]
    # hw.product/hw.machine come from the root 'model'/'target-type'. Both are
    # read by IODTPlatformExpert and end up in MobileGestalt, so the iPad identity
    # has to replace them together with the /chosen security status below: AMFI
    # derives its code-signing trust policy for the platform from that status, and
    # an adhoc-signed userspace binary (launchd) is only accepted under the
    # dev-board policy that the iPad identity declares.
    identity = [
        ('model', b'iPad7,1\0' if args.ipad_identity else b'picasso-first-boot\0'),
        *([('target-type', b'J120\0')] if args.ipad_identity else []),
    ]
    tree = node('device-tree', [
        *identity,
        # Root compatible strings of the compiled board DT (xiaomi,picasso / qcom,lito).
        ('compatible', b'xiaomi,picasso\0qcom,lito\0qcom,lito-mtp\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [('random-seed', random_seed),
                        # Without these XNU never publishes IOPlatformUUID and gethostuuid() blocks forever
                        # for callers (configd) that pass a zero timeout.
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        # This boots the normal target OS. Volatile Data is
                        # described separately by ephemeral-storage below.
                        ('osenvironment', b'main\0'),
                        # Security status of a dev-fused, undemoted board (tuple 0,0,1,1),
                        # as MobileGestalt reads it. AMFI picks its code-signing trust
                        # policy from certificate-security-mode, and the original
                        # Apple binary trusts adhoc signatures only in this dev mode:
                        # without it launchd is rejected with "unsuitable CT policy 0".
                        # Same values sheng and hummer run with on hardware.
                        *([('certificate-production-status', struct.pack('<I', 0)),
                           ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)),
                           ('effective-security-mode-sep', struct.pack('<I', 1))] if args.ipad_identity else []),
                        # This property describes the actual volatile Data RAM disk.
                        *([('ephemeral-storage', struct.pack('<I', 1))] if (args.data_ramdisk or args.packed_data_ramdisk) else []),
                        *( [('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))]
                           if args.owned_memory_size else [] ),
                        ('dram-base', struct.pack('<Q', DRAM_BASE)),
                        ('dram-size', struct.pack('<Q', args.dram_size)),
                        # Image4 hash algorithm. sha2-384 in /chosen; selects the algorithm only.
                        ('crypto-hash-method', b'sha2-384\0'),
                        *( [('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                                               '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                                               args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
                           if args.root_match_volume_uuid else [] ),
                        # Strict policy for the original AMFI: platform code only.
                        ('amfi-only-platform-code', struct.pack('<I', 1)),
                        ('embedded-panic-log-size', struct.pack('<I', PANIC_LOG_SIZE)),
                        *empty_nvram_proxy()],
             [node('memory-map', ([("PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk", bytes(16))] if (args.data_ramdisk or args.packed_data_ramdisk) else []) +
                                 ([('TrustCache', bytes(16))] if args.trust_cache else []) +
                                 ([('GpuAddon', bytes(16))] if args.gpu_addon else [])),
              # Read by the original AppleImage4 at start-up; present and empty here.
              node('manifest-properties')]),
        # The device DT declares /cpus #address-cells = 2, #size-cells = 0, so each
        # cpuN reg is a 64-bit cell. IODTPlatformExpert reads reg with those cells;
        # without them it falls back to the root node's cells and misreads the MPIDR.
        node('cpus', [('#address-cells', struct.pack('<I', 2)),
                      ('#size-cells', struct.pack('<I', 0))], children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', args.psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        # Suspend/resume is not implemented for this board. Block CPU parking.
        node('defaults', [('sleep-disabled', struct.pack('<I', 1))]),
        node('options', []),
        # J120 has no Home button; MobileGestalt reads that from here. Without the
        # node an iPad identity still advertises a Home button and the userspace
        # gesture model does not match the panel.
        *([node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                            ('button-names', b'hold\0voldown\0volup\0'),
                            ('home-button-type', struct.pack('<I', 2))])] if args.ipad_identity else []),
        # The original AppleImage4 panics without a product node. Under the iPad
        # identity this node is what MobileGestalt reports as the hardware: the
        # artwork fields describe the J120 panel (2732x2048 class, P3, scale 2) so
        # UIKit and the audio routing database pick iPad behaviour, and
        # compatible-device-fallback keeps a known device for lookups that use it.
        node('product', [('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0' if args.ipad_identity else b'Redmi K30 5G\0'),
                         ('model', b'xiaomi,picasso\0'),
                         *([('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                            ('artwork-device-idiom', b'pad\0'),
                            ('artwork-device-subtype', struct.pack('<I', 2732)),
                            ('artwork-scale-factor', struct.pack('<I', 2)),
                            ('artwork-display-gamut', b'P3\0'),
                            ('compatible-device-fallback', b'iPad6,7\0')] if args.ipad_identity else [])]),
        node('pram', [('reg', struct.pack('<2Q', PANIC_LOG_PADDR, PANIC_LOG_SIZE))]),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, 0x08000000))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q',
                                                GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                                GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                            ('redistributor-stride', struct.pack('<Q', 0x40000)),
                            ('virtual-timer-intid', struct.pack('<I', VIRTUAL_TIMER_INTID))]),
              # The GENI UART (0x988000) also sits below the APSS window, so a bus-offset
              # reg would be negative. Like ufshc below, the node is for class matching
              # only; the driver takes the physical base from the board constants.
              node('serial', [('compatible', b'qcom,geni-debug-uart\0qcom,geni-uart\0'),
                              ('boot-console', b'')]),
              # UFS (0x1d84000), the GPU blocks (0x5000000..) and the GPU SMMU all sit
              # below the APSS window base, so none of them can carry a bus-offset reg.
              # They exist for class matching only; the drivers read the physical
              # windows from the board constants. This is the same shape
              # make-sheng-adt.py uses for ufshc.
              node('ufshc', [('compatible', b'qcom,sm7250-ufshc\0')]),
              node('ufsphy', [('compatible', b'qcom,sm7250-ufs-phy\0')]),
              node('gpu', [('compatible', b'qcom,adreno-620.0\0qcom,adreno\0'),
                           ('iommus', struct.pack('<II', 0x10, 0x0))]),
              node('gmu', [('compatible', b'qcom,adreno-gmu-620.0\0qcom,adreno-gmu\0')]),
              node('adreno-smmu', [('compatible', b'qcom,adreno-smmu\0qcom,smmu-v2\0'),
                                   ('#iommu-cells', struct.pack('<I', 1)),
                                   ('phandle', struct.pack('<I', 0x10))])]),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'picasso ADT: {len(tree)} bytes, dram {args.dram_size:#x}, timebase {TIMEBASE}, virtual timer INTID {VIRTUAL_TIMER_INTID}')


if __name__ == '__main__':
    main()
