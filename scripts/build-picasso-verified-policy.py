#!/usr/bin/env python3
"""Build the signed Picasso root policy from the already-built userdata image.

Picasso's boot RAM is too small for a per-4-KiB hash table (about 70 MiB).
This tool embeds an owner-signed SHA-256 table for 128-KiB authenticated
records instead, so boot performs no full-image scan while every UFS record
read is checked before any bytes reach IOStorageFamily.

The generator hashes the exact root image, signs only the compact manifest
header with RSA-3072, verifies the result independently with OpenSSL, and
emits a Mach-O data object. It never writes the root image or a partition.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
import uuid


HEADER_BYTES = 160
SIGNATURE_BYTES = 384
KEY_BYTES = 395
BLOCK_BYTES = 4096
AUTH_RECORD_BYTES = 128 * 1024
KNOWN_ROOT_IMAGE_SHA256 = (
    'fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093'
)
ROLES = (
    {
        'name': 'OS',
        'directory': 'os',
        'volume_uuid': uuid.UUID('04769D26-2CA0-4825-B443-E2E5F88300F2'),
        'size': 6077580800,
        'sha256': 'e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2',
    },
    {
        'name': 'SystemOS',
        'directory': 'systemos',
        'volume_uuid': uuid.UUID('8846FE78-2910-4297-AF97-D59BB6F8FC16'),
        'size': 3303014400,
        'sha256': 'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f',
    },
)


def find_tree_root():
    root = Path(__file__).resolve().parents[1]
    if not (root / 'build/xnu').is_dir() and (root / 'out/picasso/tree/build/xnu').is_dir():
        root = root / 'out/picasso/tree'
    return root


ROOT = find_tree_root()


def default_image():
    configured = os.environ.get('PICASSO_ROOT_IMAGE')
    candidates = [
        Path(configured) if configured else None,
        ROOT / 'out/picasso/userdata/userdata-root.img',
    ]
    current = ROOT
    for _ in range(7):
        candidates.append(current / 'UniXNU/out/picasso/userdata/userdata-root.img')
        current = current.parent
    for candidate in candidates:
        if candidate is not None and candidate.is_file():
            return candidate
    return ROOT / 'out/picasso/userdata/userdata-root.img'


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=default_image(),
                        help='exact userdata-root.img; defaults to the existing build output')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/verified-root-picasso',
                        help='ignored policy/audit output')
    parser.add_argument('--keys', type=Path, default=ROOT / 'build/verified-root-host/keys',
                        help='ignored RSA owner key directory')
    parser.add_argument('--verified-root', type=Path, default=ROOT / 'kits/VerifiedRoot',
                        help='VerifiedRoot checkout')
    parser.add_argument('--expected-image-sha256', default=KNOWN_ROOT_IMAGE_SHA256)
    return parser.parse_args()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def regular_file(path, label):
    try:
        mode = path.stat().st_mode
    except FileNotFoundError:
        raise SystemExit(f'{label} does not exist: {path}')
    if not stat.S_ISREG(mode):
        raise SystemExit(f'{label} must be a regular file: {path}')


def parse_root_manifest(image):
    with image.open('rb') as stream:
        block = stream.read(BLOCK_BYTES)
    if len(block) != BLOCK_BYTES or block[:8] != b'SHNGROOT':
        raise SystemExit('root image does not begin with a complete SHNGROOT manifest')
    version, count, image_bytes = struct.unpack_from('<IIQ', block, 8)
    if version != 1 or count != len(ROLES) or image_bytes != image.stat().st_size:
        raise SystemExit('root manifest version/count/image length is not the expected Picasso layout')
    if hashlib.sha256(block[:4064]).digest() != block[4064:]:
        raise SystemExit('root manifest self-hash is invalid')

    windows = []
    for index, expected in enumerate(ROLES):
        entry = block[24 + index * 128:24 + (index + 1) * 128]
        name = entry[:32].split(b'\0', 1)[0]
        offset, size = struct.unpack_from('<QQ', entry, 32)
        sector_bytes, sectors = struct.unpack_from('<II', entry, 48)
        digest = entry[56:88].hex()
        if (name != expected['name'].encode() or size != expected['size'] or
                digest != expected['sha256'] or sector_bytes != 512 or sectors != 1):
            raise SystemExit(f'root manifest {expected["name"]} identity differs from the qualified image')
        padded = (size + BLOCK_BYTES - 1) & ~(BLOCK_BYTES - 1)
        if offset < BLOCK_BYTES or offset & (BLOCK_BYTES - 1) or offset + padded > image_bytes:
            raise SystemExit(f'root manifest {expected["name"]} bounds are invalid')
        windows.append((expected, index, offset, size, digest))
    return image_bytes, hashlib.sha256(block).hexdigest(), windows


def ensure_owner_key(keys):
    keys.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(keys, 0o700)
    private = keys / 'owner.pem'
    public = keys / 'owner-public.pem'
    if not private.exists():
        fd = os.open(private, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        subprocess.run(
            ['openssl', 'genpkey', '-algorithm', 'RSA',
             '-pkeyopt', 'rsa_keygen_bits:3072', '-out', str(private)],
            check=True, capture_output=True,
        )
    if not public.exists():
        subprocess.run(
            ['openssl', 'pkey', '-in', str(private), '-pubout', '-out', str(public)],
            check=True, capture_output=True,
        )
    os.chmod(private, 0o600)
    os.chmod(public, 0o644)
    return private, public


def make_chunk_manifest(source, size, private_key, public_key, anchor, expected, output):
    """Hash and sign 128-KiB records from the current source position."""
    output.mkdir(parents=True, exist_ok=True)
    table = output / 'hashes.bin'
    image_hash, table_hash = hashlib.sha256(), hashlib.sha256()
    count = 0
    with table.open('wb') as hashes:
        remaining = size
        while remaining:
            data = source.read(min(AUTH_RECORD_BYTES, remaining))
            if len(data) != min(AUTH_RECORD_BYTES, remaining):
                raise SystemExit(f'{expected["name"]} root image ended during chunk hashing')
            digest = hashlib.sha256(data).digest()
            image_hash.update(data)
            table_hash.update(digest)
            hashes.write(digest)
            remaining -= len(data)
            count += 1
    header = struct.pack(
        '<8s4I2Q16s32s32s32sQ',
        b'VROOT01\0', 1, HEADER_BYTES, AUTH_RECORD_BYTES, 0,
        size, count, expected['volume_uuid'].bytes,
        image_hash.digest(), table_hash.digest(),
        hashlib.sha256(anchor).digest(), 1,
    )
    if len(header) != HEADER_BYTES:
        raise SystemExit('internal error: signed header has the wrong size')
    (output / 'header.bin').write_bytes(header)
    (output / 'trusted-key.bin').write_bytes(anchor)
    signature = output / 'signature.bin'
    subprocess.run(
        ['openssl', 'dgst', '-sha256', '-sign', str(private_key),
         '-out', str(signature), str(output / 'header.bin')],
        check=True,
    )
    signature_bytes = signature.read_bytes()
    if len(signature_bytes) != SIGNATURE_BYTES:
        raise SystemExit(f'{expected["name"]} RSA signature has the wrong size')
    subprocess.run(
        ['openssl', 'dgst', '-sha256', '-verify', str(public_key),
         '-signature', str(signature), str(output / 'header.bin')],
        check=True, capture_output=True,
    )
    manifest = output / 'manifest.bin'
    with manifest.open('wb') as envelope:
        envelope.write(header)
        envelope.write(signature_bytes)
        with table.open('rb') as hashes:
            for data in iter(lambda: hashes.read(1024 * 1024), b''):
                envelope.write(data)
    (output / 'header-signature.bin').write_bytes(header + signature_bytes)
    return {
        'version': 1,
        'auth_record_bytes': AUTH_RECORD_BYTES,
        'image_bytes': size,
        'block_count': count,
        'volume_uuid': str(expected['volume_uuid']).upper(),
        'generation': 1,
        'image_sha256': image_hash.hexdigest(),
        'hash_table_sha256': table_hash.hexdigest(),
        'trusted_key_sha256': hashlib.sha256(anchor).hexdigest(),
        'signature_algorithm': 'RSA3072-PKCS1v1.5-SHA256',
        'signature_verified_by_openssl': True,
        'image_modified': False,
    }


def verify_manifest(policy_dir, public_key, expected):
    manifest = policy_dir / 'manifest.bin'
    with manifest.open('rb') as stream:
        header_signature = stream.read(HEADER_BYTES + SIGNATURE_BYTES)
    header = header_signature[:HEADER_BYTES]
    signature = header_signature[HEADER_BYTES:]
    if len(header_signature) != HEADER_BYTES + SIGNATURE_BYTES:
        raise SystemExit('generated policy is shorter than its signed header')
    if header[:8] != b'VROOT01\0':
        raise SystemExit('generated policy has the wrong VerifiedRoot magic')
    record_bytes, reserved = struct.unpack_from('<II', header, 16)
    image_bytes, block_count = struct.unpack_from('<QQ', header, 24)
    generation = struct.unpack_from('<Q', header, 152)[0]
    if (record_bytes != AUTH_RECORD_BYTES or reserved or image_bytes != expected['size'] or
            block_count != (image_bytes + AUTH_RECORD_BYTES - 1) // AUTH_RECORD_BYTES or
            uuid.UUID(bytes=header[40:56]) != expected['volume_uuid'] or
            header[56:88].hex() != expected['sha256'] or generation != 1):
        raise SystemExit(f'generated {expected["name"]} header identity is invalid')
    table = policy_dir / 'hashes.bin'
    if table.stat().st_size != block_count * 32:
        raise SystemExit('generated hash table length is invalid')
    expected_manifest_bytes = HEADER_BYTES + SIGNATURE_BYTES + block_count * 32
    if manifest.stat().st_size != expected_manifest_bytes:
        raise SystemExit('generated policy manifest length is invalid')
    with manifest.open('rb') as stream:
        stream.seek(HEADER_BYTES + SIGNATURE_BYTES)
        table_matches = stream.read() == table.read_bytes()
    if not table_matches:
        raise SystemExit('generated manifest does not contain its hash table')
    table_digest = sha256_file(table)
    if bytes.fromhex(table_digest) != header[88:120]:
        raise SystemExit('generated hash table digest differs from its signed header')
    signed_header = policy_dir / 'header.bin'
    signature_file = policy_dir / 'signature.bin'
    if signed_header.read_bytes() != header or signature_file.read_bytes() != signature:
        raise SystemExit('generated header/signature split is inconsistent')
    subprocess.run(
        ['openssl', 'dgst', '-sha256', '-verify', str(public_key),
         '-signature', str(signature_file), str(signed_header)],
        check=True, capture_output=True,
    )
    header_signature_path = policy_dir / 'header-signature.bin'
    header_signature_path.write_bytes(header_signature)
    return {
        'image_bytes': image_bytes,
        'auth_record_bytes': record_bytes,
        'block_count': block_count,
        'volume_uuid': str(expected['volume_uuid']).upper(),
        'generation': generation,
        'image_sha256': header[56:88].hex(),
        'hash_table_sha256': table_digest,
        'signed_header_signature_sha256': hashlib.sha256(header_signature).hexdigest(),
        'header_signature_bytes': len(header_signature),
        'manifest_bytes': expected_manifest_bytes,
        'openssl_signature_verified': True,
    }


def write_policy_assembly(output, anchor, role_files):
    assembly = output / 'policy.S'
    lines = [
        '// Generated by scripts/build-picasso-verified-policy.py.\n',
        '.section __DATA_CONST,__const\n',
        '.balign 8\n',
        '.globl _verified_root_key\n',
        '_verified_root_key:\n',
        f'.incbin "{anchor}"\n',
    ]
    for role, manifest, _ in role_files:
        lines.extend([
            '.balign 8\n',
            f'.globl _verified_root_{role}_manifest\n',
            f'_verified_root_{role}_manifest:\n',
            f'.incbin "{manifest}"\n',
        ])
    lines.extend([
        '.balign 8\n',
        '.globl _verified_root_policies\n',
        '_verified_root_policies:\n',
    ])
    for role, _, size in role_files:
        lines.extend([
            f'.quad _verified_root_{role}_manifest\n',
            f'.quad {size}\n',
        ])
    lines.extend([
        '.balign 8\n',
        '.globl _verified_root_policy_count\n',
        '_verified_root_policy_count:\n',
        f'.quad {len(role_files)}\n',
    ])
    assembly.write_text(''.join(lines))
    return assembly


def main():
    args = parse_args()
    image = args.image.expanduser().resolve()
    output = args.output.expanduser().resolve()
    keys = args.keys.expanduser().resolve()
    verified_root = args.verified_root.expanduser().resolve()
    regular_file(image, 'root image')
    regular_file(verified_root / 'tools/manifest.py', 'VerifiedRoot signer')

    image_bytes, manifest_sha256, windows = parse_root_manifest(image)
    image_sha256 = sha256_file(image)
    if image_sha256.lower() != args.expected_image_sha256.lower():
        raise SystemExit(
            f'root image SHA-256 mismatch: expected {args.expected_image_sha256}, got {image_sha256}'
        )

    private_key, public_key = ensure_owner_key(keys)
    spec = importlib.util.spec_from_file_location(
        'picasso_verified_root_manifest', verified_root / 'tools/manifest.py'
    )
    signer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(signer)
    anchor = signer.public_anchor(public_key)
    if len(anchor) != KEY_BYTES:
        raise SystemExit('the owner public key did not encode as a 395-byte VerifiedRoot anchor')

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=output.name + '.', dir=output.parent) as temporary:
        staging = Path(temporary)
        records = []
        role_files = []
        with image.open('rb') as source:
            for expected, _, offset, size, _ in windows:
                policy_dir = staging / expected['directory']
                source.seek(offset)
                record = make_chunk_manifest(
                    source, size, private_key, public_key, anchor, expected, policy_dir,
                )
                if record['image_sha256'] != expected['sha256']:
                    raise SystemExit(
                        f'{expected["name"]} data hash differs from the qualified root manifest'
                    )
                verified = verify_manifest(policy_dir, public_key, expected)
                verified.update({
                    'name': expected['name'],
                    'offset': offset,
                    'size': size,
                    'manifest_sha256': sha256_file(policy_dir / 'manifest.bin'),
                })
                records.append(verified)
                role_files.append(expected['directory'])
        anchor_path = staging / 'trusted-key.bin'
        anchor_path.write_bytes(anchor)
        if output.exists():
            shutil.rmtree(output)
        staging.rename(output)

    final_roles = [
        (role, output / role / 'manifest.bin',
         (output / role / 'manifest.bin').stat().st_size)
        for role in role_files
    ]
    assembly = write_policy_assembly(
        output, output / 'trusted-key.bin', final_roles
    )
    object_path = output / 'policy.o'
    subprocess.run(
        ['xcrun', 'clang', '-target', 'arm64-apple-ios17.0',
         '-c', str(assembly), '-o', str(object_path)],
        check=True,
    )
    symbols = subprocess.check_output(['xcrun', 'nm', '-g', str(object_path)], text=True)
    for symbol in ('_verified_root_key', '_verified_root_policies',
                   '_verified_root_policy_count', '_verified_root_os_manifest',
                   '_verified_root_systemos_manifest'):
        if symbol not in symbols:
            raise SystemExit(f'generated policy object is missing {symbol}')

    attestation = {
            'schema': 'picasso-verified-root-policy-v1',
            'root_image': {
                'path': str(image),
                'bytes': image_bytes,
                'sha256': image_sha256,
                'manifest_sha256': manifest_sha256,
            },
            'trust': {
                'algorithm': 'RSA3072-PKCS1v1.5-SHA256',
                'minimum_generation': 1,
                'trusted_key_sha256': hashlib.sha256(anchor).hexdigest(),
                'owner_public_key_sha256': sha256_file(public_key),
                'private_key_ignored': str(private_key),
            },
            'windows': records,
            'kernel_embedded': {
                'auth_record_bytes': AUTH_RECORD_BYTES,
                'bytes_per_window': {
                    record['name']: record['manifest_bytes'] for record in records
                },
                'hash_tables_embedded': True,
                'runtime_scan_required': False,
            },
            'policy_assembly_sha256': sha256_file(assembly),
            'policy_object_sha256': sha256_file(object_path),
            'root_image_modified': False,
            'partition_write_operations': False,
            'hardware_tested': False,
            'boot_root_authenticated': False,
    }
    (output / 'attestation.json').write_text(
        json.dumps(attestation, indent=2) + '\n'
    )

    print(json.dumps({
        'output': str(output),
        'root_image_sha256': image_sha256,
        'policy_assembly': str(output / 'policy.S'),
        'windows': [
            {
                'name': record['name'],
                'volume_uuid': record['volume_uuid'],
                'blocks': record['block_count'],
                'auth_record_bytes': record['auth_record_bytes'],
                'image_sha256': record['image_sha256'],
                'hash_table_sha256': record['hash_table_sha256'],
            }
            for record in records
        ],
    }, indent=2))


if __name__ == '__main__':
    main()
