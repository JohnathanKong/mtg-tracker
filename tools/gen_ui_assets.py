#!/usr/bin/env python3
"""Install image-generated, independently layered MTG HUD art at native size.

Run: python3 tools/gen_ui_assets.py main

This deliberately contains *no shape drawing*. The transparent gilded art
comes from source image-generation PNGs, cropped/masked/downsampled during
art-pack preparation, then shipped as exact 360px RGBA8888 sprites.
Firmware builds only extract those verified sprites; no scaling or painting.
The ZIP can be replaced to update artwork without changing any C++ logic.
"""
from __future__ import annotations
import hashlib
import json
import pathlib
import struct
import sys
import zipfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
PACK = ROOT / 'assets' / 'mtg_ornate_firmware_assets.zip'
SIZES = {
    'hud_chrome': (360, 360),
    'player_normal': (68, 30),
    'player_selected': (68, 30),
    'counter_teal': (106, 25),
    'counter_brass': (106, 25),
    'counter_teal_selected': (106, 25),
    'counter_brass_selected': (106, 25),
    'pencil': (33, 25),
    'editor': (288, 244),
    'editor_row_checked': (250, 39),
    'editor_row_unchecked': (250, 39),
    'checkbox_checked': (20, 20),
    'checkbox_unchecked': (20, 20),
}


def png_bytes(rgba: bytes, w: int, h: int) -> bytes:
    """Produce a QA-only PNG using Python stdlib (not embedded in firmware)."""
    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return struct.pack('!I', len(payload)) + body + struct.pack('!I', zlib.crc32(body) & 0xffffffff)
    raw = b''.join(b'\0' + rgba[y*w*4:(y+1)*w*4] for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('!2I5B', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main() -> None:
    output = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'main'
    output.mkdir(parents=True, exist_ok=True)
    if not PACK.is_file():
        raise FileNotFoundError(f'Artwork pack missing: {PACK}. Use the published repository asset pack.')
    with zipfile.ZipFile(PACK) as archive:
        meta = json.loads(archive.read('manifest.json'))
        dimensions = meta['dimensions']
        if set(dimensions) != set(SIZES):
            raise ValueError('Artwork manifest does not match the firmware sprite set')
        for name, (w, h) in SIZES.items():
            if tuple(dimensions[name]) != (w, h):
                raise ValueError(f'Incorrect dimensions for {name}: {dimensions[name]}')
            raw = archive.read(name + '.rgba')
            if len(raw) != w * h * 4:
                raise ValueError(f'Invalid {name} sprite size: {len(raw)} bytes')
            (output / (name + '.rgba')).write_bytes(raw)
            if name == 'hud_chrome':
                (output / 'hud_chrome_preview.png').write_bytes(png_bytes(raw, w, h))
    print('Loaded 13 independent high-fidelity UI assets:',
          hashlib.sha256(PACK.read_bytes()).hexdigest()[:12])


if __name__ == '__main__':
    main()