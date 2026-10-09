#!/usr/bin/env python3
"""Reference decoder tests, including frames emitted by real C transport tests."""
import argparse
import json
from pathlib import Path
import random
import struct
import unittest
import ble_reply_client as codec

HEADER = struct.Struct('<2sBBIHH')
def frame(flag=1, gen=2, total=32, offset=0, payload=b''):
    return HEADER.pack(b'\0R', 1, flag, gen, total, offset) + payload


class CodecTests(unittest.TestCase):
    def test_classifier_disjoint(self):
        snapshot = bytes([10,29,30,2,0,0,0,0,100,1,1,154])
        self.assertEqual(codec.classify(snapshot).kind, 'legacy')
        self.assertEqual(codec.classify(b'pong').kind, 'text')
        self.assertEqual(codec.classify(b'{"x":"UTF-8 \xc3\xa9"}').kind, 'text')
        self.assertEqual(codec.classify(frame()).kind, 'metadata')
        self.assertEqual(codec.classify(frame(2,payload=b'123')).kind, 'page')
        self.assertEqual(codec.classify(frame(0x81)).kind, 'error')
        for invalid in (b'\0R', frame()[:11], b'\0Xbad', b'\xffgarbage', b'\0pong', snapshot[:-1]+b'\0'):
            with self.subTest(invalid=invalid):
                self.assertEqual(codec.classify(invalid).kind, 'invalid')

    def test_count_collision_text(self):
        # Includes allowed leading whitespace, JSON/arrays and every ASCII lead.
        for lead in (*b'\t\r\n', *range(32, 127)):
            value = bytes([lead]) + b'b' * (lead + 1)
            with self.subTest(lead=lead):
                self.assertEqual(codec.classify(value), codec.Packet('text', payload=value))
        # Even a coincidentally matching legacy checksum must remain text.
        value = b'{' + b'x' * 119 + '€'.encode('utf-8') + b'!j'
        self.assertEqual(codec.reduce(codec.xor, value[1:-1], 0) ^ 0xff, value[-1])
        self.assertEqual(codec.classify(value).kind, 'text')

    def test_legacy_outbound_schema(self):
        def legacy(screen, menu, size, command=0x1d):
            value = bytes([size - 2, command, screen, menu]) + b'\0' * (size - 5)
            return value + bytes([codec.reduce(codec.xor, value[1:], 0) ^ 0xff])
        # These are exactly the layouts accepted by dura_legacy_serialize_snapshot.
        shapes = [(0x1e, 2, 12), (0, 1, 6), (0, 2, 6), (0, 0x2c, 9), (0, 0x38, 9)]
        shapes += [(screen, menu, 9) for screen in (0x50, 0x5a) for menu in range(256)]
        for screen, menu, size in shapes:
            with self.subTest(screen=screen, menu=menu, size=size):
                value = legacy(screen, menu, size)
                self.assertEqual(codec.classify(value), codec.Packet('legacy', payload=value))
                self.assertEqual(codec.classify(value[:-1] + bytes([value[-1] ^ 1])).kind, 'invalid')
        for value in (legacy(0x1e, 3, 12), legacy(0, 0, 6), legacy(1, 2, 9),
                      legacy(0x1e, 2, 9), legacy(0, 1, 9), legacy(0x50, 2, 12),
                      legacy(0x1e, 2, 12, command=0x1c), legacy(0x1e, 2, 13),
                      bytes([1, 10, 245])):
            self.assertEqual(codec.classify(value).kind, 'invalid', value)

    def test_production_short_captures(self):
        captures = {}
        for line in (EVIDENCE/'short.log').read_text().splitlines():
            if line.startswith('SHORT '):
                _, name, wire_hex = line.split()
                captures[name] = bytes.fromhex(wire_hex)
        self.assertEqual(set(captures), {'generic_unknown', 'app_unknown_json', 'valid_json_125'})
        for name, value in captures.items():
            with self.subTest(capture=name):
                self.assertEqual(len(value), 103 if name == 'generic_unknown' else 125)
                self.assertEqual(value[0] + 2, len(value))
                if 'json' in name:
                    json.loads(value)
                self.assertEqual(codec.classify(value), codec.Packet('text', payload=value))

    def test_header_validation(self):
        for invalid in (frame(gen=0), frame(total=8192), frame(4), frame(offset=1), frame(payload=b'x'), frame(2,offset=32,payload=b'x'), frame(2,offset=33), frame(2), frame(0x81,payload=b'x'), frame()[:2]+b'\2'+frame()[3:]):
            self.assertEqual(codec.classify(invalid).kind, 'invalid', invalid)
        self.assertEqual(codec.classify(frame(2,offset=32)).kind,'page')
        self.assertEqual(codec.classify(frame(0x82,offset=65535)).kind,'error')

    def test_requests(self):
        self.assertEqual(codec.metadata_command(),b'reply')
        self.assertEqual(codec.page_command(0x123456ab,0x12ef),b'reply 123456ab 12ef')
        self.assertEqual(len(codec.page_command(0xffffffff,8191)),19)
        for g,o in ((0,0),(2**32,0),(-1,0),(1,-1),(1,65536)):
            with self.assertRaises(ValueError):codec.page_command(g,o)

    def test_loss_duplicates_misorder_and_interleaving(self):
        a=codec.ReplyAssembler(codec.classify(frame(total=6)))
        self.assertFalse(a.feed(b'pong'))
        self.assertFalse(a.feed(bytes([10,29,30,2,0,0,0,0,100,1,1,154])))
        a.feed(frame(2,total=6,offset=3,payload=b'def'))
        a.feed(frame(2,total=6,offset=3,payload=b'def'))
        self.assertEqual(a.next_offset(),0)
        self.assertIsNone(a.result())
        a.feed(frame(2,total=6,offset=0,payload=b'abc'))
        self.assertEqual(a.result(),b'abcdef')
        self.assertIsNone(a.next_offset())
        a.feed(frame(total=6))  # duplicate metadata never erases collected bytes
        self.assertEqual(a.result(),b'abcdef')
        with self.assertRaises(ValueError):a.feed(frame(2,total=6,payload=b'AX'))

    def test_stale_never_resets_or_merges(self):
        a=codec.ReplyAssembler(codec.classify(frame(total=6)))
        a.feed(frame(2,total=6,payload=b'abc'))
        for packet in (frame(gen=1,total=6),frame(gen=3,total=6),frame(2,gen=3,total=6,payload=b'def'),frame(0x81,gen=3,total=4),frame(total=7)):
            with self.assertRaises(codec.StaleReply):a.feed(packet)
            self.assertEqual(a.next_offset(),3)
        for flag in (0x82,0x83):
            with self.assertRaises(codec.ReplyError):a.feed(frame(flag,total=6))
        a.feed(frame(2,total=6,offset=3,payload=b'def'))
        self.assertEqual(a.result(),b'abcdef')

    def test_production_packets(self):
        self.assertIsNotNone(EVIDENCE,'pass --evidence C test output directory')
        for mtu in (23,247,256,517):
            lines=(EVIDENCE/f'mtu{mtu}.log').read_text().splitlines()
            _,gen,_,got_mtu,_,total=lines[0].split()
            self.assertEqual(int(got_mtu),mtu)
            packets=[bytes.fromhex(x) for x in lines[1:-1]]
            a=codec.ReplyAssembler(codec.classify(frame(gen=int(gen),total=int(total))))
            dropped=packets.pop(len(packets)//2)
            packets+=packets[::7]
            random.Random(20260915).shuffle(packets)
            for packet in packets:
                self.assertLessEqual(len(packet),min(mtu-3,512))
                self.assertTrue(a.feed(packet))
                self.assertFalse(a.feed(bytes([10,29,30,2,0,0,0,0,100,1,1,154])))
            self.assertIsNone(a.result())
            self.assertEqual(a.next_offset(),codec.classify(dropped).offset)
            a.feed(dropped)
            self.assertEqual(a.result(),bytes(32+i%95 for i in range(8191)))


EVIDENCE=None
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path);args,remaining=p.parse_known_args();EVIDENCE=args.evidence
    if EVIDENCE is None:
        import subprocess, sys
        root=Path(__file__).resolve().parents[1]
        EVIDENCE=root/'build-host-tests/ble-reply-client'
        subprocess.run([sys.executable, str(root/'scripts/test_ble_reply_paging.py'), '--out', str(EVIDENCE)], check=True)
    unittest.main(argv=[__file__,*remaining])
