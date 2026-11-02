"""Exercise the actual webpage parser using packets from the firmware's C encoder."""
import io
import random
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
import stream_server as s


class ChunkSerial:
    def __init__(self, raw, seed=1):
        self.raw = io.BytesIO(raw)
        self.rng = random.Random(seed)
        self.calls = 0

    def read(self, n):
        self.calls += 1
        if self.calls % 13 == 0:
            return b""  # serial timeout with a partially buffered record
        return self.raw.read(min(n, self.rng.randint(1, 199)))


class RadioTests(unittest.TestCase):
    def setUp(self):
        s.latest = s.Latest()

    def receive(self, wire, seed=1):
        rd = s.RadioReader(ChunkSerial(wire, seed), 85)
        got = []
        for _ in range(10000):
            value = rd.read_frame()
            if value is not None:
                got.append(value)
            if rd.ser.raw.tell() == len(wire) and not rd.wire:
                break
        return got

    def test_c_encoder_gap_and_chunked_uart(self):
        raw = Path(__file__).with_name("c_uart_fixture.bin").read_bytes()
        for seed in range(1, 21):
            s.latest = s.Latest()
            frames = self.receive(b"[RF433 RX] READY\r\n"+raw, seed)
            self.assertEqual(len(frames), 1)
            payload,w,h,fmt = frames[0]
            self.assertEqual((w,h,fmt,len(payload)), (160,120,1,19200))
            self.assertEqual(payload, bytes((i*17)&255 for i in range(19200)))
            self.assertEqual(s.latest.radio_incomplete, 1)
            self.assertEqual(s.latest.bad_crc, 0)

    def test_bad_uart_checksum_then_recover(self):
        raw = Path(__file__).with_name("c_uart_fixture.bin").read_bytes()
        damaged = bytearray(raw)
        damaged[40] ^= 1
        got = self.receive(bytes(damaged)+raw)
        self.assertEqual(len(got), 2)
        self.assertGreater(s.latest.bad_crc, 0)

    def test_early_end_and_oversize_header(self):
        def record(frame, size, flags):
            data = b"OV3S"+struct.pack("<HHBBI",160,120,0,1,size)+b"fake"
            packet=struct.pack("<2sBBIIIHBB",b"RI",1,1,999,frame,0,len(data),flags,0)+data
            return b"RFI1"+struct.pack("<H",len(packet))+packet+struct.pack("<I",sum(packet))
        raw = Path(__file__).with_name("c_uart_fixture.bin").read_bytes()
        got = self.receive(record(9,1000,3)+record(10,0xffffffff,1)+raw)
        self.assertEqual(len(got),1)
        self.assertGreater(s.latest.radio_incomplete,0)


if __name__ == "__main__":
    unittest.main()
