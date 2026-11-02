"""Read connected image hardware without flashing; preserve raw evidence."""
import argparse
import hashlib
import io
import json
import re
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import serial
from PIL import Image
import stream_server as server


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--stm32', default='COM30')
    ap.add_argument('--tx', default='COM34')
    ap.add_argument('--rx', default='COM20')
    a = ap.parse_args()
    out = Path(__file__).resolve().parents[1] / 'hardware_check'
    out.mkdir(exist_ok=True)
    wires = {k: bytearray() for k in ('stm32', 'tx', 'rx')}
    ports = {'stm32': (a.stm32, 1000000), 'tx': (a.tx, 115200), 'rx': (a.rx, 921600)}
    handles, errors = {}, []
    stop = threading.Event()

    def read(name, ser):
        try:
            while not stop.is_set():
                wires[name].extend(ser.read(max(1, min(65536, ser.in_waiting))))
        except Exception as e:
            errors.append(name+': '+str(e))

    for name, (port, baud) in ports.items():
        ser = serial.Serial(port, baud, timeout=0.1, write_timeout=1)
        ser.set_buffer_size(rx_size=262144)
        handles[name] = ser
    threads = [threading.Thread(target=read, args=(name, ser)) for name, ser in handles.items()]
    for thread in threads:
        thread.start()
    try:
        # Query an idempotent known setting. The baseline prints a confirmation;
        # experiment firmware locks legacy settings and answers E4 instead.
        handles['stm32'].write(b'Q12\nT1\n')
        handles['stm32'].flush()
        start = time.monotonic()
        while time.monotonic()-start < a.seconds:
            stop.wait(min(1, a.seconds-(time.monotonic()-start)))
    finally:
        stop.set()
        for thread in threads:
            thread.join(2)
        for ser in handles.values():
            ser.close()

    class Replay:
        def __init__(self, wire):
            self.raw = io.BytesIO(wire)
        @property
        def in_waiting(self):
            return len(self.raw.getbuffer())-self.raw.tell()
        def read(self, n):
            return self.raw.read(n)

    summaries, all_frames = {}, {}
    for name, wire in wires.items():
        (out / (name+'_60s.bin')).write_bytes(wire)
        if name == 'tx':
            summaries[name] = dict(bytes=len(wire), log=wire.decode('utf-8', 'replace'))
            continue
        server.latest = server.Latest()
        replay = Replay(wire)
        reader = (server.Reader if name == 'stm32' else server.RadioReader)(replay, 85)
        frames = []
        while True:
            before = replay.raw.tell(), len(reader.buf), len(getattr(reader, 'wire', b''))
            result = reader.read_frame()
            if result:
                payload, w, h, fmt = result
                if fmt == 0:
                    image = Image.open(io.BytesIO(payload))
                    image.load()
                    assert image.size == (w, h)
                    (out / (name+'_verified.jpg')).write_bytes(payload)
                frames.append(dict(seq=reader.last_seq, width=w, height=h, format=fmt,
                                   size=len(payload), sha256=hashlib.sha256(payload).hexdigest()))
            after = replay.raw.tell(), len(reader.buf), len(getattr(reader, 'wire', b''))
            if not result and before == after:
                break
        lines = re.findall(rb'\[(?:RF433[^\]]*|PERF|TEST)\][^\r\n]*', wire)
        summaries[name] = dict(frames=len(frames), bytes=len(wire),
                               fps=round(len(frames)/a.seconds, 3),
                               bad_checksum=server.latest.bad_crc,
                               incomplete=server.latest.radio_incomplete,
                               sequence_gaps=server.latest.dropped,
                               dimensions=sorted(set((f['width'], f['height'], f['format']) for f in frames)),
                               telemetry=[line.decode('utf-8', 'replace') for line in lines])
        all_frames[name] = frames
    hashes = {(v['seq'], v['sha256']) for v in all_frames['stm32']}
    matched = [v for v in all_frames['rx'] if (v['seq'], v['sha256']) in hashes]
    raw = bytes(wires['stm32'])
    command_confirmed = b'JPEG ' in raw and b'12' in raw
    result = dict(checked_utc=datetime.now(timezone.utc).isoformat(), seconds=a.seconds, ports=ports,
                  summary=summaries, byte_identical_frames=len(matched),
                  radio_frames=len(all_frames['rx']), serial_errors=errors,
                  test_firmware_telemetry=b'[TEST]' in raw,
                  legacy_quality_confirmation=command_confirmed,
                  firmware_flashed=False,
                  limits=['No physical cold boot/power disconnection or instrument power measurement performed'],
                  frames=all_frames)
    (out / 'sustained_check.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'frames'}, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    main()
