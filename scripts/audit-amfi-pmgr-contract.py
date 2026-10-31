#!/usr/bin/env python3
"""Audit the unchanged target AMFI -> PMGR boundary; no hardware access."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import backed_offset, exports, macho


def require(ok, message):
    if not ok:
        raise SystemExit(message)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--candidate', type=Path,
                    default=ROOT.parent / 'kernel/kernel.macho')
parser.add_argument('--original', type=Path)
args = parser.parse_args()
if not args.candidate.exists():
    # Keep supporting the upstream prelink layout when this audit is run from a
    # tree that still stores the native candidate there.
    later = sorted((ROOT / 'build/native-prelink').glob(
        'native-owner-ufs-cpu-desktop-*-candidate.macho'), key=lambda f: f.stat().st_mtime)
    require(later, 'No reference candidate available')
    args.candidate = later[-1]
data = args.candidate.read_bytes()
plan = json.loads(args.candidate.with_suffix('.json').read_text())
meta = macho(data)
symbols = exports(data, meta)


def read(va, size):
    at = backed_offset(meta['segments'], va, size, len(data))
    return data[at:at + size]


def words(va, expected):
    require(struct.unpack('<' + 'I' * len(expected), read(va, 4 * len(expected))) ==
            expected, f'Instruction contract changed at {va:#x}')


def pointer(va):
    return struct.unpack('<Q', read(va, 8))[0]


hashes = {
    'com.apple.driver.AppleMobileFileIntegrity':
        'a1088aaabc259418ed503d08df29b6146470b8c962696c688122a1f60a141db6',
    'com.apple.driver.ApplePMGR':
        '2c433279d004803b7ef02948e550ad6b77aa3f84b02a06a357e00fa738310a99',
}
for module, digest in hashes.items():
    extent, = [e for e in plan['extents'] if e['module'] == module and
               e['segment'] == '__TEXT_EXEC']
    require(hashlib.sha256(read(extent['va'], extent['size'])).hexdigest() ==
            plan['original_executable_sha256'][module] == digest,
            f'Original executable differs: {module}')

# Check actual indirect imports, rather than treating a diagnostic string as an ABI.
for slot, name in (
    (0xfffffff005cfad48, '__ZN9IOService15serviceMatchingEPKcP12OSDictionary'),
    (0xfffffff005cfad50, '__ZN9IOService22waitForMatchingServiceEP12OSDictionaryy'),
    (0xfffffff005cfac68, '__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass'),
):
    require(name in symbols.get(pointer(slot), []), 'Native IOKit import differs')
require(read(0xfffffff0042dde02, 13) == b'ApplePMGRNub\0', 'Matching class differs')
require(pointer(0xfffffff005cfac08) == 0xfffffff004a24adc and
        pointer(0xfffffff005cfac10) == 0xfffffff004a230ec, 'PMGR imports differ')

# AMFI: read, OR bit 2, write. The imported methods dispatch through the
# provider stored at nub+0x88; the slots belong to that provider, not the nub.
words(0xfffffff004aaeb40, (0x94006c46, 0x321e0015, 0xaa1403e0,
                         0xaa1503e1, 0x94006c45))
words(0xfffffff004a24adc, (0xf9404400, 0xb4000080, 0xf9400008,
                         0xf9450101, 0xd61f0020, 0x12800000, 0xd65f03c0))
words(0xfffffff004a230ec, (0xf9404400, 0xb4000080, 0xf9400008,
                         0xf9450502, 0xd61f0040, 0xd65f03c0))


def constructor_vptr(reader, pc):
    first, second, store = struct.unpack('<3I', reader(pc, 12))
    require(first & 0x9f00001f == 0x90000008 and
            second & 0xffc003ff == 0x91000108 and store == 0xf9000008,
            'Constructor must form x8 with ADRP/ADD and store it at this+0')
    imm = ((first >> 5) & 0x7ffff) << 2 | ((first >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~4095) + (imm << 12) + ((second >> 10) & 4095)


base_vptr = constructor_vptr(read, 0xfffffff0049f0058)
require(base_vptr == 0xfffffff005ce26d8 and
        struct.unpack('<2Q', read(base_vptr + 320 * 8, 16)) ==
        (0xfffffff004a20fbc, 0xfffffff004a20fc4), 'Base provider slots differ')
words(0xfffffff004a20fbc, (0x12800000, 0xd65f03c0, 0xd65f03c0))

# The target-specific driver is inspected in the original supplied cache; it
# is not in this native candidate. Do not infer hardware semantics from its name.
original_path = args.original
if original_path is None:
    for candidate in (
        ROOT / '.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed',
        ROOT.parents[1] / 'ipsw/21H461/kernelcache.decompressed',
    ):
        if candidate.is_file():
            original_path = candidate
            break
require(original_path is not None, 'No original iPad7,1 kernelcache available')
original = original_path.read_bytes()
original_meta = macho(original)


def original_read(va, size):
    at = backed_offset(original_meta['segments'], va, size, len(original))
    return original[at:at + size]


header = backed_offset(original_meta['segments'], 0xfffffff00584f680, 32, len(original))
target = macho(original, header)
target_hashes = {
    '__TEXT': 'bd4be83b7c24b18d4796be180e8bdaf96a2a4670407e85101164c991cd9d408c',
    '__TEXT_EXEC': '59d36c869db750dcde3ec1ab87d26619beef7c12d929d674e0b3d38357ad5b22',
    '__DATA_CONST': '429c542da926dfa6376ba1dc6e622f2a3cb6afa3c0bfade43d4954c2c63a2a07',
}
for name, digest in target_hashes.items():
    segment, = [s for s in target['segments'] if s['name'] == name]
    require(hashlib.sha256(original_read(segment['va'], segment['filesize'])).hexdigest() == digest,
            f'Target AppleT8011PMGR {name} differs')
require(original_read(0xfffffff005852125, 15) == b'AppleT8011PMGR\0', 'Target class name differs')
target_vptr = constructor_vptr(original_read, 0xfffffff006a55fb0)
require(target_vptr == 0xfffffff006f9da90, 'Target constructor vptr differs')
target_slots = struct.unpack('<2Q', original_read(target_vptr + 320 * 8, 16))
require(target_slots == (0xfffffff006a577e8, 0xfffffff006a577f0), 'Target PMGR slots differ')
require(struct.unpack('<2I', original_read(target_slots[0], 8)) == (0x12800000, 0xd65f03c0) and
        struct.unpack('<I', original_read(target_slots[1], 4)) == (0xd65f03c0,),
        'Target must return 0xffffffff on read and immediately return on write')

report = {
    'evidence': 'static analysis of original executable bytes and rebound imports',
    'original_executable_sha256': hashes,
    'service_class': 'ApplePMGRNub', 'provider_field_offset': '0x88',
    'provider_read_vtable_byte_offset': '0xa00', 'provider_read_slot': 320,
    'provider_write_vtable_byte_offset': '0xa08', 'provider_write_slot': 321,
    'amfi_value_update': '32-bit read result OR 0x4, then passed to write',
    'null_provider': 'read returns 0xffffffff; write returns without an operation',
    'target_provider': {
        'class': 'AppleT8011PMGR', 'original_segment_sha256': target_hashes,
        'constructor_vptr': hex(target_vptr), 'slot_targets': [hex(v) for v in target_slots],
        'read': 'returns 0xffffffff', 'write': 'returns immediately without MMIO',
        'runtime_invocation_observed': False,
        'included_in_native_candidate': 'com.apple.driver.AppleT8011PMGR' in plan['components'],
    },
    'limitations': [
        'Target constant-read/no-op-write behavior is static evidence, not runtime service qualification',
        'No Snapdragon provider lifecycle, service matching or platform power operations qualified',
        'The PMGRAON log is not an independent latch readback',
        'No replacement nub, MMIO access, VM declaration or security bypass installed',
        'No QEMU execution or hardware observation is produced by this audit',
    ],
}
out = ROOT / 'build/amfi-pmgr-contract/report.json'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(report, indent=2) + '\n')
print('PASS: unchanged AMFI read/OR/write and original PMGR provider dispatch')
print('PASS: actual target AppleT8011PMGR slots read 0xffffffff and perform no write')
print('Static evidence only; native platform service integration remains unresolved')
