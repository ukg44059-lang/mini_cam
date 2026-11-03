"""Offline HTTP regression with simulated radio replies, no hardware/current data."""
import json
import math
import re
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path
import test_console as tc
import stream_server as server


def run():
    checks = []
    with tempfile.TemporaryDirectory() as folder:
        tc.OUT = Path(folder)
        tc.experiments = server.experiments = tc.Experiments()
        exp = tc.experiments

        class FakeSerial:
            auto_ack, case, rid = True, 1, 0

            def __init__(self):
                self.commands = []

            def write(self, data):
                assert data.startswith(b'WRC1')
                size = int.from_bytes(data[4:6], 'little')
                command = data[6:6+size].decode('ascii')
                self.commands.append(command)
                for key, dest in [('A', 'rid'), ('E', 'case')]:
                    match = re.search(key+r'(\d+)\n', command)
                    if match:
                        setattr(self, dest, int(match[1]))
                if self.auto_ack:
                    self.ack()

            def ack(self, **overrides):
                standby, sleep, image = tc.EXPECTED[self.case]
                fields = dict(proto=4, rid=self.rid, case=self.case, led=0, interval_ms=1000,
                              standby=standby, sleep=sleep, image=image, fault=0, skipped=0)
                fields.update(overrides)
                exp.telemetry(('[TEST] event=status '+' '.join('%s=%s'%i for i in fields.items())+'\n').encode())

            def flush(self):
                pass

        serial = FakeSerial()
        server.latest.ser = serial
        server.latest.firmware_mode = 'test'
        server.latest.receiver['source'] = 'rf433'
        serial.ack()
        http = ThreadingHTTPServer(('127.0.0.1', 0), server.Handler)
        threading.Thread(target=http.serve_forever, daemon=True).start()
        base = 'http://127.0.0.1:%d' % http.server_port

        def post(**q):
            req = urllib.request.Request(base+'/test/action', json.dumps(q).encode(),
                                         {'Content-Type': 'application/json'})
            with urllib.request.urlopen(req) as r:
                return json.load(r)

        def reject(**q):
            try:
                post(**q)
            except urllib.error.HTTPError as e:
                assert e.code == 400
                return
            raise AssertionError('Expected HTTP 400: '+str(q))

        def record_ready():
            exp.active['confirmed_at'] = time.monotonic()-36

        try:
            with urllib.request.urlopen(base) as r:
                html = r.read().decode()
            assert all('start(%d)'%c in html for c in tc.CASES)
            assert 'start(5)' not in html and '无线芯片不进入休眠' in html
            checks.append('四档入口、无线不休眠说明')
            for case, ma in [(1, 100), (2, 70), (3, 45), (4, 30)]:
                result = post(action='start', case=case)
                assert result['active']['status'] == 'running'
                assert 'E%d\n'%case in serial.commands[-1]
                assert 'L' not in serial.commands[-1] and 'I' not in serial.commands[-1]
                reject(action='record', voltage_v=5, current_ma=ma)
                record_ready()
                if case == 1:
                    reject(action='record', voltage_v=5, current_ma=ma)
                    exp.frame(640, 480, 0)
                    reject(action='record', voltage_v=5, current_ma=ma)
                    exp.frame(480, 360, 0)
                post(action='record', voltage_v=5, current_ma=ma)
            assert exp.readings[0]['power_mw'] == 500
            assert [r['delta_current_ma'] for r in exp.readings[1:]] == [30, 25, 15]
            checks.append('四档映射、板端确认后计时、有效图像门控及V×mA功率与差值')
            for v, i in [(0, 1), (5, -1), (float('nan'), 1), (5, float('inf'))]:
                reject(action='record', voltage_v=v, current_ma=i)
            checks.append('拒绝非法电表读数')
            post(action='start', case=2);record_ready()
            post(action='record', voltage_v=6, current_ma=60)
            assert 'delta_current_ma' not in exp.readings[-1]
            exp.telemetry(b'[TEST] event=frame case=2 bytes=0 skipped=0\n')
            post(action='record', voltage_v=5, current_ma=60)
            assert not exp.readings[-1]['comparable']
            assert 'delta_current_ma' not in exp.readings[-1]
            checks.append('电压不一致或采集异常时不计算差值')
            serial.auto_ack = False
            post(action='start', case=3)
            a = exp.active
            reject(action='start', case=4)
            serial.ack(rid=serial.rid-1)
            serial.ack(proto=3)
            serial.ack(sleep=1)
            serial.ack(fault=1)
            assert a['status'] == 'pending'
            a['ack_deadline'] = 0
            assert exp.state()['active']['status'] == 'unconfirmed'
            serial.ack()
            assert a['status'] == 'running'
            confirmed = a['confirmed_at']
            serial.ack()
            assert confirmed == a['confirmed_at']
            checks.append('拒绝旧请求/旧协议/错误状态/故障回执；超时后有效确认；查询不重置计时')
            serial.ack(rid=0)
            assert a['status'] == 'desynced'
            reject(action='record', voltage_v=5, current_ma=30)
            serial.auto_ack = True
            post(action='start', case=4)
            exp.board['received_at'] = time.time()-20
            reject(action='record', voltage_v=5, current_ma=30)
            reject(action='start', case=1)
            serial.ack()
            checks.append('板端重启/失配及状态过期禁止记录')
            serial.auto_ack = False
            post(action='stop')
            exp.active['ack_deadline'] = 0
            assert exp.state()['active']['status'] == 'stop_failed'
            serial.ack()
            assert exp.active['status'] == 'stopped'
            serial.auto_ack = True
            checks.append('暂停未收到确认不报成功，迟到的匹配暂停回执可恢复')
            exp.board['proto'] = 3
            reject(action='start', case=1)
            serial.ack()
            reject(action='start', case=5)
            checks.append('旧版八项固件和旧测试编号拒绝')
            with urllib.request.urlopen(base+'/test/export') as r:
                exported = json.load(r)
            assert len(exported['readings']) == 6 and exported['schema'] == 'four_mode_v1'
            restored = tc.Experiments()
            assert len(restored.readings) == 6 and restored.active['status'] == 'stopped'
            post(action='start', case=1)
            restored = tc.Experiments()
            assert restored.active['status'] == 'interrupted'
            old_send = server.latest.send_cmd
            server.latest.send_cmd = lambda _: 'disconnected'
            reject(action='stop')
            server.latest.send_cmd = old_send
            assert exp.active['status'] == 'stop_failed' and not exp.active.get('ended')
            checks.append('导出/落盘/重启恢复/写入失败保护')
            post(action='new_group')
            assert not exp.readings
            assert len(list(Path(folder).glob('20*.json'))) > 4
            checks.append('新测量组保留历史文件')
        finally:
            http.shutdown()
            http.server_close()
    report = dict(ok=True, offline=True, simulated_meter_values=True, checks=checks,
                  hardware_tested=False, current_measured=False)
    out = Path(__file__).resolve().parents[1]/'validation/offline_validation.json'
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    run()
