#!/usr/bin/env python3
"""Exercise the actual loader parser/copy routine and the Python packer."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile

from packed_ramdisk import inspect, pack, regions

header = Path(__file__).resolve().parents[1] / 'DarwinBootPkg/Driver/PackedRamdisk.h'
with tempfile.TemporaryDirectory(prefix='darwinboot-packed-') as temporary:
    work = Path(temporary)
    source = work / 'test.c'
    source.write_text('''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef int BOOLEAN;
#define STATIC static
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
#define SIZE_256MB 0x10000000ULL
#define SIZE_2GB 0x80000000ULL
#define EFI_PAGE_SIZE 4096
#define CompareMem memcmp
#define CopyMem memcpy
static uint32_t ReadUnaligned32(const uint32_t *p) {uint32_t v; memcpy(&v,p,4);return v;}
static uint64_t ReadUnaligned64(const uint64_t *p) {uint64_t v; memcpy(&v,p,8);return v;}
#include "PackedRamdisk.h"
int validate(void *p, size_t n, size_t *expanded) {
  BOOLEAN packed; return RamdiskLayout(p,n,expanded,&packed);
}
int decode(void *out, size_t capacity, void *p, size_t n) {
  BOOLEAN packed; size_t expanded;
  if (!RamdiskLayout(p,n,&expanded,&packed) || expanded>capacity) return 0;
  memset(out,0,expanded); CopyRamdisk(out,p,n,packed); return 1;
}
''')
    library = work / 'parser.dylib'
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    '-I', str(header.parent), str(source), '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    lib.validate.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    lib.decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]

    def valid(blob):
        size = ctypes.c_size_t()
        result = lib.validate(ctypes.create_string_buffer(blob), len(blob), ctypes.byref(size))
        return result, size.value

    raw = b'head' + bytes(4092) + bytes(8192) + b'tail' + bytes(4092)
    (work / 'raw').write_bytes(raw)
    pack(work / 'raw', work / 'packed')
    blob = (work / 'packed').read_bytes()
    assert valid(blob) == (1, len(raw))
    out = ctypes.create_string_buffer(len(raw))
    assert lib.decode(out, len(raw), ctypes.create_string_buffer(blob), len(blob)) == 1
    assert out.raw == raw
    assert inspect(work / 'packed')[0]['bytes'] == len(raw)
    maximum = struct.pack('<8sIIQQ', b'DBRDISK1', 1, 4096, 1024**4, 0)
    assert valid(maximum) == (1, 1024**4)
    assert regions(maximum) == (1024**4, [])
    invalid = [blob[:31], blob[:-1], blob + b'x']
    for offset, format_, value in [(8, '<I', 2), (12, '<I', 16384),
                                   (16, '<Q', 0), (16, '<Q', 1024**4 + 4096),
                                   (24, '<Q', 65537), (32, '<Q', 4097),
                                   (40, '<Q', 0), (40, '<Q', 0xfffffffffffff000),
                                   (48, '<Q', 0), (48, '<Q', 16384)]:
        changed = bytearray(blob)
        struct.pack_into(format_, changed, offset, value)
        invalid.append(changed)
    for bad in invalid:
        assert not valid(bytes(bad))[0]
        try:
            regions(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('Python accepted malformed input rejected by C')
    print(f'PASS: actual C parser/copy, sparse round-trip, 1 TiB boundary, {len(invalid)} refusal cases')
