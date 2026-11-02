"""Offline regression: real HTTP handlers, mock serial, boot gating and energy."""
import json
import math
import tempfile
import threading
import urllib.request
import urllib.error
from pathlib import Path
from http.server import ThreadingHTTPServer

import test_console as tc
import stream_server as server


def run():
    checks = []
    class FakeSerial:
        def __init__(self):
            self.commands = []
        def write(self, data):
            self.commands.append(data.decode())
        def flush(self):
            pass
    serial = FakeSerial()
    latest = server.latest
    latest.ser = serial
    latest.receiver['source'] = 'rf433'
    http = ThreadingHTTPServer(('127.0.0.1', 0), server.Handler)
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    base = 'http://127.0.0.1:%d' % http.server_port

    def post(data):
        req = urllib.request.Request(base+'/test/action', json.dumps(data).encode(),
                                     {'Content-Type': 'application/json'})
        with urllib.request.urlopen(req) as r:
            return json.load(r)

    with tempfile.TemporaryDirectory() as folder:
        tc.OUT = Path(folder)
        try:
            with urllib.request.urlopen(base) as r:
                html = r.read().decode()
                assert all('start(%d)' % c in html for c in tc.CASES)
            checks.append('首页包含8个测试入口')
            for case in range(2, 9):
                post(dict(action='start', case=case, duty=35, interval=10, phase='active'))
                command = serial.commands[-1]
                assert 'E%d\n' % (4 if case == 8 else case) in command
                if case == 5:
                    assert 'L35\n' in command
                post(dict(action='stop'))
                assert serial.commands[-1] == 'E0\n'
            checks.append('②至⑧命令映射及停止下发')
            # Neither an old image before marker nor a wrong-size image counts.
            for _ in range(5):
                post(dict(action='start', case=1))
                before = len(tc.experiments.boots)
                tc.experiments.frame(480, 360, 0)
                assert len(tc.experiments.boots) == before
                post(dict(action='power_on'))
                tc.experiments.frame(640, 480, 0)
                assert len(tc.experiments.boots) == before
                tc.experiments.frame(480, 360, 0)
                assert len(tc.experiments.boots) == before+1
            checks.append('上电5次有效图像门控与错误分辨率拒绝')
            try:
                post(dict(action='start', case=1))
                raise AssertionError('第6次应拒绝')
            except urllib.error.HTTPError as e:
                assert e.code == 400
            post(dict(action='samples', channel='tx', samples=[
                dict(t_s=0, voltage_v=3.3, current_a=0.02),
                dict(t_s=10, voltage_v=3.3, current_a=0.03)]))
            assert math.isclose(tc.experiments.active['energy_j']['tx'], 0.825)
            checks.append('仪表梯形积分0.825 J')
            for samples in ([dict(t_s=0, voltage_v=3.3, current_a=float('nan'))],
                            [dict(t_s=0, voltage_v=3.3, current_a=0.1)]*2):
                try:
                    post(dict(action='samples', samples=samples))
                    raise AssertionError('应拒绝无效采样')
                except urllib.error.HTTPError as e:
                    assert e.code == 400
            checks.append('拒绝非有限采样及重复时间')
            with urllib.request.urlopen(base+'/test/export') as r:
                export = json.load(r)
                assert len(export['boots']) == 5
                assert export['active']['samples']['tx'][1] == [10.0, 3.3, 0.03]
            assert len(list(Path(folder).glob('*.json'))) == 12
            checks.append('HTTP导出与每次实验独立落盘')
            tc.experiments.action(dict(action='reset_boots'), latest)
            tc.experiments.action(dict(action='start', case=6), latest)
            tc.experiments.telemetry(b'[RF433 TX] NO_ACK retries=2 ack_failures=3\r\n')
            assert 'NO_ACK' in tc.experiments.active['events'][-1]['text']
            checks.append('无接收机日志进入测试记录')
        finally:
            http.shutdown()
            http.server_close()
    report = dict(ok=True, offline=True, checks=checks,
                  hardware_tested=False, firmware_flashed=False)
    out = Path(__file__).resolve().parents[1] / 'validation' / 'offline_validation.json'
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    run()
