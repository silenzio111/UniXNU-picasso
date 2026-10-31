#!/usr/bin/env python3
"""Generate freely redistributable Mach-O/ADT fixtures; no Apple binaries."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess

BASE = 0xfffffff007004000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--memory-size', type=lambda value: int(value, 0), choices=[0x10000000, 0x20000000])
    parser.add_argument('--gop', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tools = {name: shutil.which(name) for name in ('clang', 'ld.lld', 'llvm-objcopy')}
    if not all(tools.values()):
        parser.error('LLVM and lld are required on PATH')
    subprocess.run([tools['clang'], '--target=aarch64-none-elf', '-c',
                    f'-DFIXTURE_WINDOW_SIZE={args.memory_size or 0x10000000}',
                    f'-DFIXTURE_GOP={int(args.gop)}',
                    f'-DFIXTURE_ADT_EXTRA={44 if args.memory_size else 0}',
                    str(Path(__file__).with_name('Kernel.S')), '-o', str(output / 'kernel.o')], check=True)
    subprocess.run([tools['ld.lld'], f'-Ttext={BASE + 0x1000:#x}', '-e', '_start',
                    str(output / 'kernel.o'), '-o', str(output / 'kernel.elf')], check=True)
    subprocess.run([tools['llvm-objcopy'], '-O', 'binary', str(output / 'kernel.elf'),
                    str(output / 'kernel-code.bin')], check=True)
    code = (output / 'kernel-code.bin').read_bytes()
    if len(code) > 0x1000:
        raise SystemExit('Fixture exceeds the synthetic TEXT segment')

    def segment(name, address, size, offset, length, protection):
        return struct.pack('<II16sQQQQiiII', 0x19, 72, name, address, size, offset, length,
                           protection, protection, 0, 0)

    state = bytearray(272)
    struct.pack_into('<Q', state, 256, BASE + 0x1000)
    commands = (segment(b'__TEXT', BASE, 0x2000, 0, 0x2000, 5)
                + segment(b'__DATA', BASE + 0x4000, 0x1000, 0, 0, 3)
                + struct.pack('<IIII', 5, 288, 6, 68) + state)
    image = bytearray(0x2000)
    image[:32] = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 3, len(commands), 0, 0)
    image[32:32 + len(commands)] = commands
    image[0x1000:0x1000 + len(code)] = code
    tree = struct.pack('<II32sI12s', 1, 0, b'name', 12, b'device-tree\0')
    def node(name, properties=(), children=()):
        props = [('name', name.encode() + b'\0'), *properties]
        data = struct.pack('<II', len(props), len(children))
        for key, value in props:
            data += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
        return data + b''.join(children)

    def ramdisk_tree(value, duplicate=False):
        memory = node('memory-map', [('RAMDisk', value)])
        return node('device-tree', children=[node('chosen', children=[memory] * (2 if duplicate else 1))])

    def trust_tree(value, duplicate=False):
        memory = node('memory-map', [('TrustCache', value)])
        return node('device-tree', children=[node('chosen', children=[memory] * (2 if duplicate else 1))])

    empty_tree = tree
    tree = node('device-tree', children=[node('chosen',
        [('darwinboot-memory-size', struct.pack('<Q', args.memory_size))] if args.memory_size else [], children=[
        node('memory-map', [('RAMDisk', bytes(16)), ('TrustCache', bytes(16))])])])
    extra_trees = {'missing-ramdisk-adt': empty_tree,
                   'stale-ramdisk-adt': ramdisk_tree(struct.pack('<QQ', 0x1000, 0x1000)),
                   'bad-ramdisk-size': ramdisk_tree(bytes(8)),
                   'duplicate-ramdisk': ramdisk_tree(bytes(16), duplicate=True),
                   'missing-trustcache': trust_tree(bytes(16)),
                   'orphan-trustcache': empty_tree,
                   'stale-trustcache': trust_tree(struct.pack('<QQ', 0x1000, 56)),
                   'duplicate-trustcache': trust_tree(bytes(16), duplicate=True),
                   **{name: trust_tree(bytes(16)) for name in
                      ('bad-trustcache-version', 'bad-trustcache-count',
                       'bad-trustcache-offset', 'bad-trustcache-order')}}
    for name, properties in {
            'bad-memory-size': [('darwinboot-memory-size', bytes(4))],
            'unavailable-memory-window': [('darwinboot-memory-size', struct.pack('<Q', 0x40000000))],
            'small-memory-window': [('darwinboot-memory-size', struct.pack('<Q', 0))],
            'unaligned-memory-window': [('darwinboot-memory-size', struct.pack('<Q', 0x10001000))],
            'large-memory-window': [('darwinboot-memory-size', struct.pack('<Q', 0x200200000))],
            'duplicate-memory-window': [('darwinboot-memory-size', struct.pack('<Q', 0x20000000))] * 2,
            'misplaced-memory-window': [('darwinboot-memory-size', struct.pack('<Q', 0x20000000))]}.items():
        extra_trees[name] = (node('device-tree', properties, [node('chosen')]) if name == 'misplaced-memory-window'
                             else node('device-tree', children=[node('chosen', properties)]))
    module = struct.pack('<I16sI', 2, bytes(range(16)), 1) + bytes([0x11]) * 20 + bytes([2, 0, 0, 0])
    trustcache = struct.pack('<2I', 1, 8) + module
    trust_variants = {name: trustcache for name in ('kernel', 'orphan-trustcache', 'stale-trustcache',
                      'duplicate-trustcache', 'bad-trustcache-version', 'bad-trustcache-count', 'bad-trustcache-offset')}
    for name, offset, value in [('bad-trustcache-version', 8, 3), ('bad-trustcache-count', 28, 0xffffffff),
                               ('bad-trustcache-offset', 4, 0xfffffff0)]:
        modified = bytearray(trustcache)
        struct.pack_into('<I', modified, offset, value)
        trust_variants[name] = modified
    unordered = bytearray(trustcache + bytes([0x10]) * 20 + bytes([2, 0, 0, 0]))
    struct.pack_into('<I', unordered, 28, 2)
    trust_variants['bad-trustcache-order'] = unordered
    (output / 'ramdisk.bin').write_bytes(b'DRAMDISK' + bytes(4088) + b'\xa5')
    variants = {'kernel': bytes(image), 'bad-adt': bytes(image), 'missing-adt': bytes(image),
                **{name: bytes(image) for name in extra_trees}}
    for name, offset, fmt, value in [('bad-subtype', 8, '<I', 2),
                                     ('bad-command', 20, '<I', len(image)),
                                     ('bad-overlap', 32 + 72 + 24, '<Q', BASE + 0x1000),
                                     ('bad-entry', 32 + 144 + 272, '<Q', BASE + 0x3000)]:
        modified = bytearray(image)
        struct.pack_into(fmt, modified, offset, value)
        variants[name] = bytes(modified)
    for name, data in variants.items():
        path = output / f'{name}.macho'
        path.write_bytes(data)
        adt = path.with_suffix('.macho.adt')
        adt.unlink(missing_ok=True)
        sidecar = path.with_suffix('.macho.trustcache')
        sidecar.unlink(missing_ok=True)
        if name in trust_variants:
            sidecar.write_bytes(trust_variants[name])
        if name != 'missing-adt':
            adt.write_bytes(b'\xff' * 8 if name == 'bad-adt' else extra_trees.get(name, tree))
    print('Synthetic Mach-O/ADT fixtures generated')


if __name__ == '__main__':
    main()
