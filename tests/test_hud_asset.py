"""Validate the checked-in 360px RGBA-palette/LZ4 artwork-independent HUD.

No third-party Python packages are required by this regression test. It uses
the same bounds and token semantics as the minimal firmware decoder.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "main" / "ui_ornate_hud_360.lz4"
COLORS = 32
WIDTH = HEIGHT = 360

def decode_lz4_block(data: bytes, expected: int) -> bytes:
    out = bytearray()
    index = 0
    while index < len(data):
        token = data[index]
        index += 1
        literal = token >> 4
        if literal == 15:
            while True:
                assert index < len(data), "truncated literal length"
                extra = data[index]
                index += 1
                literal += extra
                if extra != 255:
                    break
        assert index + literal <= len(data), "truncated literal payload"
        assert len(out) + literal <= expected, "literal exceeds allocated pixels"
        out.extend(data[index:index + literal])
        index += literal
        if index == len(data):
            break
        assert index + 2 <= len(data), "missing match offset"
        distance = data[index] | (data[index + 1] << 8)
        index += 2
        assert 0 < distance <= len(out), "invalid match distance"
        length = token & 15
        if length == 15:
            while True:
                assert index < len(data), "truncated match length"
                extra = data[index]
                index += 1
                length += extra
                if extra != 255:
                    break
        length += 4
        assert len(out) + length <= expected, "match exceeds allocated pixels"
        for _ in range(length):
            out.append(out[-distance])
    assert len(out) == expected, "decoded pixel count mismatch"
    return bytes(out)

class OrnateHudTests(unittest.TestCase):
    def test_chrome_embeds_with_correct_palette_and_resolution(self):
        blob = SOURCE.read_bytes()
        self.assertGreater(len(blob), COLORS * 4)
        palette = blob[:COLORS * 4]
        self.assertEqual(palette[3], 0, "palette index 0 must be transparent")
        pixels = decode_lz4_block(blob[COLORS * 4:], WIDTH * HEIGHT)
        self.assertTrue(any(pixel != 0 for pixel in pixels), "no curves encoded")
        self.assertTrue(all(pixel < COLORS for pixel in pixels),
                        "indexed color beyond palette")
        # No baked-in caption or live number should be present in the static
        # decorative asset; only the dynamic renderer knows counter values.

if __name__ == "__main__":
    unittest.main()
