"""Validate the two flash-embedded UI resources without third-party packages."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
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
    def test_native_layers_generated_independently(self):
        script = MAIN.parent / "tools" / "gen_ui_assets.py"
        expected = {
            "hud_chrome": (360, 360),
            "player_normal": (68, 30),
            "player_selected": (68, 30),
            "counter_teal": (106, 25),
            "counter_brass": (106, 25),
            "counter_teal_selected": (106, 25),
            "counter_brass_selected": (106, 25),
            "pencil": (33, 25),
            "editor": (288, 244),
            "editor_row_checked": (250, 39),
            "editor_row_unchecked": (250, 39),
            "checkbox_checked": (20, 20),
            "checkbox_unchecked": (20, 20),
        }
        with tempfile.TemporaryDirectory() as temp:
            subprocess.run([sys.executable, str(script), temp],
                           capture_output=True, check=True, timeout=120)
            assets = {}
            for name, (width, height) in expected.items():
                raw = (Path(temp) / (name + ".rgba")).read_bytes()
                self.assertEqual(len(raw), width * height * 4, name)
                assets[name] = raw
            # No user art or baked sample numbers in the fixed chrome.
            base = assets["hud_chrome"]
            self.assertEqual(base[(25*360+180)*4+3], 0)
            self.assertGreater(base[(275*360+180)*4+3], 190)
            self.assertGreater(base[(8*360+180)*4+3], 40)
            self.assertNotEqual(assets["counter_teal"], assets["counter_brass"])
            self.assertNotEqual(assets["counter_teal"], assets["counter_teal_selected"])
            self.assertNotEqual(assets["player_normal"], assets["player_selected"])
            self.assertNotEqual(assets["editor_row_checked"],
                                assets["editor_row_unchecked"])
            self.assertNotEqual(assets["checkbox_checked"],
                                assets["checkbox_unchecked"])
            self.assertTrue((Path(temp)/"hud_chrome_preview.png").read_bytes()
                            .startswith(b"\x89PNG\r\n\x1a\n"))

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
