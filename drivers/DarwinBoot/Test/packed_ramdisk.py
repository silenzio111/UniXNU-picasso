#!/usr/bin/env python3
"""Lossless zero-page packing for the DarwinBoot DBRDISK1 input format."""
import argparse
import hashlib
from pathlib import Path
import struct

PAGE = 4096
MAX_INPUT = 256 * 1024**2
MAX_LOGICAL = 1024**4
ZERO = bytes(1024**2)


def regions(blob):
    if len(blob) < 32 or len(blob) > MAX_INPUT:
        raise ValueError('Packed input size out of bounds')
    magic, version, page, logical, count = struct.unpack_from('<8sIIQQ', blob)
    if (magic != b'DBRDISK1' or version != 1 or page != PAGE or
            not 0 < logical <= MAX_LOGICAL or logical % PAGE or
            count > 65536 or count > (len(blob) - 32) // 16):
        raise ValueError('Invalid packed RAM disk header')
    cursor = 32 + 16 * count
    end = 0
    result = []
    for index in range(count):
        offset, length = struct.unpack_from('<QQ', blob, 32 + 16 * index)
        if (not length or (offset | length) % PAGE or offset < end or
                offset > logical or length > logical - offset or length > len(blob) - cursor):
            raise ValueError('Invalid packed RAM disk extent')
        result.append((offset, memoryview(blob)[cursor:cursor + length]))
        end = offset + length
        cursor += length
    if cursor != len(blob):
        raise ValueError('Trailing packed RAM disk bytes')
    return logical, result


PLAIN_HASH_LIMIT = 2 * 1024**3


def logical_hash(logical, extents):
    if logical > PLAIN_HASH_LIMIT:
        return seed_identity(logical, extents)
    digest = hashlib.sha256()
    end = 0
    for offset, data in [*extents, (logical, b'')]:
        while end < offset:
            count = min(len(ZERO), offset - end)
            digest.update(memoryview(ZERO)[:count])
            end += count
        digest.update(data)
        end += len(data)
    return digest.hexdigest()


def seed_identity(logical, extents):
    """Identity of a disk above 2 GiB, as the native block driver computes it (drivers/NativeRamBlock/SeedDisk.h seedIdentity): the size and
    the nonzero pages with their indexes, so hashing a mostly empty disk of hundreds of GiB takes no longer than its content."""
    digest = hashlib.sha256()
    digest.update(b'SHNGSEED2' + struct.pack('<Q', logical))
    for offset, data in extents:
        for at in range(0, len(data), PAGE):
            page = bytes(data[at:at + PAGE])
            if any(page):
                digest.update(struct.pack('<Q', (offset + at) // PAGE) + page)
    return digest.hexdigest()


def inspect(path):
    size = path.stat().st_size
    if not 0 < size <= MAX_INPUT:
        raise ValueError('Encoded RAM disk exceeds 256 MiB input bound')
    blob = path.read_bytes()
    encoded_hash = hashlib.sha256(blob).hexdigest()
    if blob[:8] == b'DBRDISK1':
        logical, extents = regions(blob)
        header = bytes(PAGE)
        if extents and extents[0][0] == 0:
            header = bytes(extents[0][1][:PAGE])
        return dict(bytes=logical, sha256=logical_hash(logical, extents),
                    encoded_bytes=size, encoded_sha256=encoded_hash, packed=True), header
    if size % PAGE:
        raise ValueError('Raw RAM disk must be page aligned')
    return dict(bytes=size, sha256=encoded_hash, packed=False), blob[:PAGE]


def data_ranges(stream, logical):
    """(start, end) of the regions of the file that hold data, skipping holes (a sparse raw image of a large disk is mostly holes)."""
    import os
    fd = stream.fileno()
    position = 0
    while position < logical:
        try:
            start = os.lseek(fd, position, os.SEEK_DATA)
        except OSError:
            return
        end = min(os.lseek(fd, start, os.SEEK_HOLE), logical)
        yield start, end
        position = end


def pack(source, destination):
    logical = source.stat().st_size
    if not 0 < logical <= MAX_LOGICAL or logical % PAGE:
        raise ValueError('Raw input must be page aligned and at most 1 TiB')
    extents = []
    digest = hashlib.sha256()
    total = 32
    large = logical > PLAIN_HASH_LIMIT
    with source.open('rb') as stream:
        # Above 2 GiB only the data regions are read; the identity (seed_identity) is computed from the result below.
        def chunk_bases():
            for start, end in data_ranges(stream, logical):
                first = (start // len(ZERO)) * len(ZERO)
                for chunk_start in range(first, end, len(ZERO)):
                    yield chunk_start
        bases = chunk_bases() if large else range(0, logical, len(ZERO))
        last = -1
        for base in bases:
            if base <= last:
                continue
            last = base
            stream.seek(base)
            chunk = stream.read(min(len(ZERO), logical - base))
            if len(chunk) != min(len(ZERO), logical - base):
                raise ValueError('Raw input changed during packing')
            if not large:
                digest.update(chunk)
            for at in range(0, len(chunk), PAGE):
                page = chunk[at:at + PAGE]
                if not any(page):
                    continue
                offset = base + at
                if extents and extents[-1][0] + len(extents[-1][1]) == offset:
                    extents[-1][1].extend(page)
                else:
                    extents.append((offset, bytearray(page)))
                    total += 16
                total += PAGE
                if total > MAX_INPUT or len(extents) > 65536:
                    raise ValueError('Nonzero input exceeds packed format bound')
    with destination.open('xb') as output:
        output.write(struct.pack('<8sIIQQ', b'DBRDISK1', 1, PAGE, logical, len(extents)))
        for offset, data in extents:
            output.write(struct.pack('<QQ', offset, len(data)))
        for _, data in extents:
            output.write(data)
    metadata, _ = inspect(destination)
    if not large and metadata['sha256'] != digest.hexdigest():
        raise ValueError('Packed reconstruction differs from complete raw input hash')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    import json
    print(json.dumps(pack(args.source, args.destination), indent=2))
