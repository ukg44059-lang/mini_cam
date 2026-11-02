"""Offline regression: real HTTP handlers, mock serial, boot gating and energy."""
import json
import math
import tempfile
import time
import threading
import urllib.request
import urllib.error
from pathlib import Path
from http.server import ThreadingHTTPServer

import test_console as tc
import stream_server as server


def run():
    checks = []
    tc.experiments.boots = []
    tc.experiments.active = None
    class FakeSerial:
        def __init__(self):
            self.commands = []
        def write(self, data):
            if data.startswith(b'WRC1'):
                size = int.from_bytes(data[4:6], 'little')
                command = data[6:6+size].decode('ascii')
            else:
                command = data.decode('ascii')
            self.commands.append(command)
            if self.auto_ack:
                import re
                match = re.search(r'A(\d+)\n', command)
                if match:
                    self.rid = int(match[1])
                match = re.search(r'E(\d+)\n', command)
                if match:
                    self.case = int(match[1])
                match = re.search(r'I(\d+)\n', command)
                if match:
                    self.interval = int(match[1])*1000
                match = re.search(r'L(\d+)\n', command)
                self.duty = int(match[1]) if match else 0
                tc.experiments.telemetry(('[TEST] event=status proto=3 rid=%d case=%d led=%d interval_ms=%d standby=0\n' %
                                         (self.rid, self.case, self.duty, self.interval)).encode())
        auto_ack = True
        rid, case, duty, interval = 0, 0, 0, 10000
        def flush(self):
            pass
    serial = FakeSerial()
    latest = server.latest
    latest.ser = serial
    latest.firmware_mode = 'test'
    tc.experiments.board = dict(proto=3, received_at=time.time())
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
                assert 'E0\n' in serial.commands[-1]
                assert tc.experiments.active['status'] == 'stopped'
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
            assert len([p for p in Path(folder).glob('*.json') if not p.name.startswith('.')]) == 12
            checks.append('HTTP导出与每次实验独立落盘')
            tc.experiments.action(dict(action='reset_boots'), latest)
            tc.experiments.action(dict(action='start', case=6), latest)
            tc.experiments.telemetry(b'[RF433 TX] NO_ACK retries=2 ack_failures=3\r\n')
            assert 'NO_ACK' in tc.experiments.active['events'][-1]['text']
            checks.append('无接收机日志进入测试记录')
            post(dict(action='stop'))
            serial.auto_ack = False
            post(dict(action='start', case=3))
            a = tc.experiments.active
            assert a['status'] == 'pending'
            tc.experiments.telemetry(b'[TEST] event=status proto=2 rid=0 case=3 led=0 interval_ms=10000\n')
            assert a['status'] == 'pending', 'Old ACK must not confirm a new request'
            a['ack_deadline'] = 0
            assert tc.experiments.state()['active']['status'] == 'unconfirmed'
            serial.auto_ack = True
            post(dict(action='stop'))
            assert a['status'] == 'stopped'
            checks.append('匹配请求确认、拒绝旧确认、超时及停止确认')
            restored = tc.Experiments()
            assert len(restored.boots) == 0
            assert restored.active['status'] == 'stopped'
            tc.experiments.action(dict(action='start', case=3), latest)
            tc.experiments.save()
            restored = tc.Experiments()
            assert restored.active['status'] == 'interrupted'
            checks.append('服务重启恢复记录并标记中断')
            # A failed stop write cannot be reported as an ended test.
            old_send = latest.send_cmd
            latest.send_cmd = lambda _: 'disconnected'
            try:
                post(dict(action='stop'))
                raise AssertionError('Stop write should fail')
            except urllib.error.HTTPError as e:
                assert e.code == 400
            finally:
                latest.send_cmd = old_send
            assert tc.experiments.active['status'] == 'stop_failed'
            assert not tc.experiments.active.get('ended')
            checks.append('停止写入失败不误报已停止')

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
