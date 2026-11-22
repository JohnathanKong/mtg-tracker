"""Validate the two flash-embedded UI resources without third-party packages."""
from pathlib import Path
import struct
import unittest

MAIN = Path(__file__).resolve().parents[1] / "main"


def decode_block(src: bytes, output_bytes: int) -> bytes:
    out = bytearray()
    p = 0
    while p < len(src):
        token = src[p]
        p += 1
        literal = token >> 4
        if literal == 15:
            while True:
                if p == len(src):
                    raise AssertionError("LZ4 literal length truncated")
                n = src[p]
                p += 1
                literal += n
                if n != 255:
                    break
        assert p + literal <= len(src), "LZ4 overread"
        out.extend(src[p:p + literal])
        p += literal
        assert len(out) <= output_bytes, "LZ4 buffer overflow"
        if p == len(src):
            break
        assert p + 2 <= len(src), "LZ4 distance truncated"
        offset = src[p] + src[p + 1] * 256
        p += 2
        assert 0 < offset <= len(out), "LZ4 offset invalid"
        length = token & 15
        if length == 15:
            while True:
                assert p < len(src), "LZ4 match length truncated"
                n = src[p]
                p += 1
                length += n
                if n != 255:
                    break
        length += 4
        assert len(out) + length <= output_bytes, "LZ4 match overflow"
        for _ in range(length):
            out.append(out[-offset])
    assert len(out) == output_bytes, f"{len(out)} != {output_bytes}"
    return bytes(out)


class EmbeddedUiTests(unittest.TestCase):
    def test_ornate_chrome_exact_dimensions(self):
        content = (MAIN / "ui_reference_chrome_256.lz4").read_bytes()
        self.assertGreater(len(content), 768)
        pixels = decode_block(content[768:], 360 * 360 * 2)
        self.assertEqual(len(pixels), 259200)
        indices = pixels[::2]
        alpha = pixels[1::2]
        self.assertTrue(all(0 <= i <= 255 for i in indices))
        self.assertGreater(sum(a > 0 for a in alpha), 20000)
        self.assertLess(sum(a > 0 for a in alpha), 360 * 360)

    def test_antialiased_serif_glyphs(self):
        content = (MAIN / "ui_font_atlas.lz4").read_bytes()
        declared = struct.unpack_from("<I", content)[0]
        glyphs = decode_block(content[4:], declared)
        self.assertEqual(glyphs[:4], b"MFA1")
        self.assertEqual(glyphs[4], 3)
        offset = 5
        for face in range(3):
            size, count = glyphs[offset:offset + 2]
            offset += 2
            self.assertGreater(size, 0)
            self.assertGreaterEqual(count, 12)
            codes = []
            for _ in range(count):
                code, width, height, left, top, advance = struct.unpack_from("<BBBbbB", glyphs, offset)
                offset += 6
                codes.append(code)
                self.assertGreater(advance, 0)
                offset += (width * height + 1) // 2
                self.assertLessEqual(offset, len(glyphs))
            self.assertIn(ord("0"), codes)
            self.assertIn(ord("+"), codes)
        self.assertEqual(offset, len(glyphs))


if __name__ == "__main__":
    unittest.main()
