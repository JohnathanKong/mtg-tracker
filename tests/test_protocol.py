#!/usr/bin/env python3
"""Reference framing/CRC sanity checks for docs/sync-protocol-v1.md."""
import json
import unittest
import zlib

MAX_PAYLOAD = 65536
CHUNK = 256

def frame_document(document):
    raw = json.dumps(document, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    if not raw or len(raw) > MAX_PAYLOAD:
        raise ValueError('invalid size')
    crc = zlib.crc32(raw)
    frames = [f'MTG1 BEGIN {len(raw)} {crc:08X}\n'.encode('ascii')]
    for offset in range(0, len(raw), CHUNK):
        data = raw[offset:offset + CHUNK]
        frames += [f'MTG1 CHUNK {len(data)}\n'.encode('ascii'), data]
    frames += [b'MTG1 COMMIT\n']
    return frames, raw

class ProtocolTests(unittest.TestCase):
    def test_unicode_and_custom_trackers(self):
        profile = {
            'version': 1, 'decks': [{
                'id': 'abc', 'name': 'Drágons ⚔️',
                'commander': 'The Ur-Dragon', 'colors': ['W','U','B','R','G'],
                'trackers': ['life','poison','custom-abc'],
                'customTrackers': [{'id':'custom-abc','label':'Goblins created'}]
            }]
        }
        frames, raw = frame_document(profile)
        received = bytearray()
        for i in range(1,len(frames)-1,2):
            size=int(frames[i].decode().strip().split()[-1])
            part=frames[i+1]
            self.assertEqual(len(part),size)
            received.extend(part)
        self.assertEqual(bytes(received),raw)
        self.assertIn('MTG1 BEGIN',frames[0].decode())
        self.assertEqual(int(frames[0].decode().split()[-1],16),zlib.crc32(raw))

    def test_empty_deck_library(self):
        frames, raw = frame_document({'version':1,'decks':[]})
        self.assertEqual(json.loads(raw.decode())['decks'],[])

    def test_bad_checksum_is_detectable(self):
        _, raw = frame_document({'version':1,'decks':[{'id':'1'}]})
        damaged = bytearray(raw)
        damaged[-3] ^= 0x01
        self.assertNotEqual(zlib.crc32(raw),zlib.crc32(damaged))

if __name__ == '__main__':
    unittest.main()
