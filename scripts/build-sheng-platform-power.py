#!/usr/bin/env python3
"""Build the guarded original-PMGR lifecycle archive for Picasso.

The prelinked ApplePMGR component is byte-identical to the component used by
sheng. This builder recreates that device's audited subclass ABI, refuses to
run if the original AMFI/PMGR contract changes, and links ApplePMGR's private
read/write slots to the original implementations. It is a publication shim,
not a functional Qualcomm power backend.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'

OUT = ROOT / 'build/sheng-platform-power'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import backed_offset, exports, macho  # noqa: E402


def run(command, **kwargs):
    return subprocess.check_output([str(value) for value in command], timeout=90, **kwargs)


def require(value, message):
    if not value:
        raise SystemExit(message)


print(run(['python3', ROOT / 'scripts/audit-amfi-pmgr-contract.py'], text=True), end='')

header = '''// Generated exact storage/slot ABI; private fields stay opaque.
#pragma once
#include <IOKit/IOPMGR.h>
class ApplePMGR : public IOPMGR {
 OSDeclareAbstractStructors(ApplePMGR)
 unsigned char originalPrivate[0x4b8a8-sizeof(IOPMGR)];
public:
'''
guards = ''
for i in range(177, 340):
    if i == 320:
        header += ' virtual uint32_t readAon();\n'
    elif i == 321:
        header += ' virtual void writeAon(uint64_t);\n'
    else:
        header += f' virtual void opaque{i}() = 0;\n'
        guards += f' void opaque{i}() override {{ unavailable({i}); }}\n'
header += '};\nstatic_assert(sizeof(ApplePMGR)==0x4b8a8,"Original provider storage");\n'
(OUT / 'OriginalPMGRABI.h').write_text(header)
(OUT / 'GuardedPMGRSlots.inc').write_text(guards)

source = ROOT / 'drivers/ShengPlatformPower/ShengPlatformPower.cpp'
obj = OUT / 'ShengPlatformPower.o'
record_path = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/'
               'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
record = json.loads(record_path.read_text().strip().rstrip(','))
command = []
index = 0
arguments = record['arguments']
while index < len(arguments):
    arg = arguments[index]
    if arg == record['file']:
        command.append(str(source))
    elif arg == '-o':
        command.extend(['-o', str(obj)])
        index += 1
    elif arg == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not arg.startswith('-W'):
        command.append(arg)
    index += 1
# XNU records -nostdlibinc without a sysroot. Add only the host SDK's
# include directory while preserving the recorded iOS target and ABI.
host_sdk = subprocess.run(
    ['xcrun', '--sdk', 'macosx', '--show-sdk-path'],
    capture_output=True, text=True)
if host_sdk.returncode == 0 and host_sdk.stdout.strip():
    command.extend(['-isystem', str(Path(host_sdk.stdout.strip()) / 'usr/include')])
command.extend([
    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-Wno-c99-designator', '-Wno-reorder-init-list',
    '-Wno-deprecated-enum-enum-conversion',
    '-Wno-unnecessary-virtual-specifier',
    '-I', str(OUT), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
    '-I', str(ROOT / 'scripts'),
])
run(command, cwd=record['directory'])

raw = obj.read_bytes()
info = macho(raw)
require(info['kind'] == 1, 'Require actual ARM64 subtype-0 Mach-O object')
build_version = [
    struct.unpack_from('<2I', raw, at + 8)
    for cmd, at, _ in info['commands'] if cmd == 0x32
]
require(build_version == [(2, 0x110000)], 'Require iOS17 build target')

sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
at, count, strings, _ = struct.unpack_from('<4I', raw, sym_at + 8)
symbols = []
for i in range(count):
    index, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, at + i * 16)
    name = raw[strings + index:raw.index(b'\0', strings + index)].decode()
    symbols.append((name, address, kind))

vtable = next(va for name, va, _ in symbols if name == '__ZTV18ShengPlatformPower')
relocations = {}
for segment in info['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            at, count = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
            for i in range(count):
                offset, bits = struct.unpack_from('<2I', raw, at + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]
entries = [relocations.get(vtable + 16 + i * 8) for i in range(340)]
require(all(entries) and vtable + 16 + 340 * 8 not in relocations,
        'Exact 340-entry provider table')

candidate = ROOT.parent / 'kernel/kernel.macho'
if not candidate.is_file():
    candidate = ROOT / 'out/picasso/kernel/kernel.macho'
require(candidate.is_file(), 'No prelinked Picasso kernel candidate available')
candidate_data = candidate.read_bytes()
meta = macho(candidate_data)
native_names = {
    name: va for va, names in exports(candidate_data, meta).items() for name in names
}


def ptr(va):
    return struct.unpack_from(
        '<Q', candidate_data,
        backed_offset(meta['segments'], va, 8, len(candidate_data)),
    )[0]


bindings = {
    '__ZN9ApplePMGRC2EPK11OSMetaClass': 0xfffffff0049f003c,
    '__ZN9ApplePMGRD2Ev': 0xfffffff0049f019c,
    '__ZN9ApplePMGR10gMetaClassE': 0xfffffff006adf490,
}
for slot, name in enumerate(entries):
    if slot in (0, 1, 7) or 168 <= slot < 340 and slot not in (320, 321):
        require('18ShengPlatformPower' in name,
                f'Own guard/lifecycle required at slot {slot}')
    elif slot in (320, 321):
        require('9ApplePMGR' in name,
                'Only actual original AON implementations inherited')
        bindings[name] = ptr(0xfffffff005ce26d8 + slot * 8)
    else:
        require(
            native_names.get(name) ==
            ptr(native_names['__ZTV6IOPMGR'] + 16 + slot * 8),
            f'Native IOPMGR/IOService entry differs at {slot}',
        )

undefined = {name for name, _, kind in symbols if kind == 1 and 'ApplePMGR' in name}
require(set(bindings) == undefined, 'All and only audited original base imports')
bindings.update({
    '_sheng_original_pmgr_nub_create': 0xfffffff004a22568,
    '_sheng_original_pmgr_nub_read': 0xfffffff004a24adc,
    '_sheng_original_pmgr_nub_write': 0xfffffff004a230ec,
})

assembly = OUT / 'original-bindings.S'
assembly.write_text(''.join(
    f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in bindings.items()
))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
run([
    'xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
    '-c', assembly, '-o', OUT / 'original-bindings.o',
])

late = OUT / 'ShengPlatformPower.late.o'
run([
    'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
    '--globalize-symbol=__GLOBAL__sub_I_ShengPlatformPower.cpp', obj, late,
])


def text_section(blob):
    section = next(
        section for segment in macho(blob)['segments']
        for section in segment['sections'] if section['name'] == '__text'
    )
    return blob[section['fileoff']:section['fileoff'] + section['size']]


require(text_section(raw) == text_section(late.read_bytes()),
        'Late registration cannot change instructions')

archive = OUT / 'libShengPlatformPower.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o'])
report = {
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'candidate_sha256': hashlib.sha256(candidate_data).hexdigest(),
    'bindings': {name: hex(va) for name, va in bindings.items()},
    'virtual_entries': 340,
    'mode': 'opt-in original nub publication; unimplemented power slots stop',
    'runtime_qualified': False,
    'original_executable_modified': False,
}
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: original PMGR ABI audited and guarded archive built')
