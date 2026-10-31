#!/usr/bin/env python3
"""Remove one bundle from an experimental prelinked kernel's XML metadata."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f'cannot load {path}')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--kit', type=Path, required=True,
                        help='NativePrelink directory containing native_prelink.py and emit_prelink.py')
    parser.add_argument('--component', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    sys.path.insert(0, str(args.kit.resolve()))
    native_prelink = load_module(args.kit / 'native_prelink.py', 'native_prelink')
    emit_prelink = load_module(args.kit / 'emit_prelink.py', 'emit_prelink')

    image = bytearray(args.kernel.read_bytes())
    macho = native_prelink.macho(image)
    info = next((segment for segment in macho['segments']
                 if segment['name'] == '__PRELINK_INFO'), None)
    if info is None:
        raise SystemExit('kernel has no __PRELINK_INFO segment')
    offset = native_prelink.backed_offset(
        macho['segments'], info['va'], info['filesize'], len(image))
    metadata = native_prelink.unserialize_xml(
        bytes(image[offset:offset + info['filesize']]))
    entries = metadata.get('_PrelinkInfoDictionary')
    if not isinstance(entries, list):
        raise SystemExit('metadata has no _PrelinkInfoDictionary')
    remaining = [entry for entry in entries
                 if entry.get('CFBundleIdentifier') != args.component]
    if len(remaining) == len(entries):
        raise SystemExit(f'{args.component} is not present')
    metadata['_PrelinkInfoDictionary'] = remaining
    xml = emit_prelink.ET.tostring(
        emit_prelink.to_xml(metadata), encoding='utf-8'
    ).replace(b' />', b'/>') + b'\0'
    if len(xml) > info['filesize']:
        raise SystemExit('replacement metadata exceeds the existing segment')
    image[offset:offset + info['filesize']] = (
        xml + bytes(info['filesize'] - len(xml)))
    native_prelink.macho(image)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(json.dumps({
        'kernel': str(args.output),
        'removed': args.component,
        'remaining': len(remaining),
        'metadata_bytes': len(xml),
        'image_bytes': len(image),
    }, indent=2))


if __name__ == '__main__':
    main()
