"""Four RF-controlled modes and manually read whole-system input currents."""
import copy
import json
import math
import re
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

CASES = {1: '完整运行', 2: '不发无线图像', 3: '摄像头待机 / MCU运行', 4: '摄像头待机 / MCU Sleep'}
OUT = Path(__file__).resolve().parents[1] / 'test_results'
EXPECTED = {0: (1, 1, 0), 1: (0, 0, 1), 2: (0, 0, 0), 3: (1, 0, 0), 4: (1, 1, 0)}


class Experiments:
    def __init__(self):
        self.lock = threading.RLock()
        self.active = None
        self.boots = []  # Preserve historical eight-case exports without reinterpretation.
        self.readings = []
        self.telemetry_tail = {}
        self.board, self.tx = {}, {}
        self.request_id = int(time.time()) % 900000 + 1
        self.last_save = 0
        state = OUT / '.session_state.json'
        if state.exists():
            saved = json.loads(state.read_text(encoding='utf-8'))
            self.boots, self.readings = saved.get('boots', []), saved.get('readings', [])
            self.active = saved.get('active')
            if self.active:
                self.active['_t0'] = time.monotonic()-self.active.get('elapsed_s', 0)
                if not self.active.get('ended'):
                    self.active.update(ended=True, status='interrupted', error='服务重启，请重新确认模式')

    def save(self):
        OUT.mkdir(exist_ok=True)
        if self.active:
            self.active['elapsed_s'] = round(time.monotonic()-self.active['_t0'], 3)
            path = OUT / (self.active['id'] + '.json')
            temp = path.with_suffix('.tmp')
            temp.write_text(json.dumps(self.active, ensure_ascii=False, indent=2), encoding='utf-8')
            temp.replace(path)
        path = OUT / '.session_state.json'
        temp = path.with_suffix('.tmp')
        temp.write_text(json.dumps(dict(active=self.active, boots=self.boots, readings=self.readings),
                                   ensure_ascii=False), encoding='utf-8')
        temp.replace(path)

    def event(self, kind, **data):
        if self.active and not self.active.get('ended'):
            self.active['events'].append(dict(t_s=round(time.monotonic()-self.active['_t0'], 3),
                                               kind=kind, **data))
            self.active['events'] = self.active['events'][-1000:]

    def ready(self, latest):
        if latest.receiver['source'] != 'rf433' or latest.ser is None:
            raise ValueError('请连接接收端CC1310应用串口并选择433 MHz来源')
        if self.board.get('proto') != 6:
            raise ValueError('需要测试固件 proto=6；旧版固件不能使用本页按钮')
        if time.time()-self.board.get('received_at', 0) > 15:
            raise ValueError('15秒内没有无线板端状态，请查询状态并检查供电')

    def start(self, q, latest):
        case = int(q['case'])
        if case not in CASES:
            raise ValueError('模式编号必须为1至4')
        self.ready(latest)
        window = int(q.get('window_s', 30))
        if not 5 <= window <= 600:
            raise ValueError('观察时间为5至600秒')
        if self.active and self.active.get('status') in ('pending', 'stopping'):
            raise ValueError('正在等待上一条无线确认，请稍候或查询状态')
        self.request_id = self.request_id % 999999 + 1
        if self.active and not self.active.get('ended'):
            self.event('mode_switch_requested', next_case=case)
            self.active.update(ended=True, status='switched')
            self.save()
        feature_isp = bool(self.board.get('isp', 1))
        feature_jpeg = bool(self.board.get('jpeg', 1))
        self.active = dict(
            schema='four_mode_v1', id=datetime.now().strftime('%Y%m%d_%H%M%S_')+uuid.uuid4().hex[:6],
            case=case, name=CASES[case], request_id=self.request_id, expected_case=case,
            status='pending', ack_deadline=time.monotonic()+10, _t0=time.monotonic(),
            started_utc=datetime.now(timezone.utc).isoformat(), settle_s=5, window_s=window,
            expected_isp=int(feature_isp), expected_jpeg=int(feature_jpeg),
            settings=dict(width=480, height=360, jpeg_qs=12, isp=feature_isp,jpeg=feature_jpeg,
                          interval_ms=1000, led=0,
                          rf_mhz=433.92, rate_kbps=500, tx_power_register='0x913F', rf_control=True),
            events=[], frames=0, readings=[], health_errors=0,
            baseline=latest.snapshot_stats(), note=str(q.get('note', ''))[:1000])
        self.event('command_sent')
        error = latest.send_cmd('A%d\nE%d\nH0\n' % (self.request_id, case))
        if error:
            self.active.update(ended=True, status='failed', error=error)
            self.save()
            raise ValueError(error)
        self.save()
        return self.state()

    @staticmethod
    def matches(fields, target, isp=True, jpeg=True):
        return (fields.get('proto') == 6 and fields.get('case') == target and
                fields.get('led') == 0 and fields.get('interval_ms') == 1000 and
                fields.get('width') == 480 and fields.get('height') == 360 and
                fields.get('fps_milli') == 1000 and fields.get('isp') == int(isp) and
                fields.get('jpeg') == int(jpeg) and fields.get('qs') == 12 and
                fields.get('pipeline') == 1 and
                fields.get('fault') == 0 and
                tuple(fields.get(k) for k in ('standby', 'sleep')) == EXPECTED[target][:2] and
                fields.get('image') == int(target == 1 and jpeg))

    def telemetry(self, raw, source='stm32'):
        with self.lock:
            lines = (self.telemetry_tail.get(source, b'')+raw).split(b'\n')
            self.telemetry_tail[source] = lines.pop()[-2048:]
            for line in lines:
                text = line.decode('utf-8', 'replace').strip()
                fields = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', text)}
                a = self.active
                if '[RF433 TX]' in text:
                    self.tx.update(fields, last=text)
                if '[TEST]' in text and 'event=status' in text:
                    self.board = dict(fields, received_at=time.time())
                    if a and not a.get('ended'):
                        target = 0 if a['status'] in ('stopping', 'stop_failed') else a['expected_case']
                        matched = (fields.get('rid') == a['request_id'] and
                                   self.matches(fields, target, a.get('expected_isp', True),
                                                a.get('expected_jpeg', True)))
                        if matched and a['status'] in ('pending', 'unconfirmed', 'stopping', 'stop_failed'):
                            self.event('board_confirmed', **fields)
                            if target == 0:
                                a.update(ended=True, status='stopped')
                            else:
                                a.update(status='running', confirmed_at=time.monotonic(),
                                         confirmed_utc=datetime.now(timezone.utc).isoformat())
                                a.pop('error', None)
                            self.save()
                        elif a['status'] == 'running' and not matched:
                            a.update(status='desynced', error='板端重启、模式或参数改变，请重新选择模式')
                            self.event('board_mismatch', **fields)
                            self.save()
                if a and a.get('status') == 'running' and '[TEST] event=frame' in text:
                    if fields.get('case') == a['case'] and (fields.get('bytes') == 0 or fields.get('skipped', 0)):
                        a['health_errors'] += 1
                if '[TEST]' in text or '[RF433' in text:
                    self.event('board', text=text)
            if self.active and not self.active.get('ended') and time.monotonic()-self.last_save > 5:
                self.save()
                self.last_save = time.monotonic()

    def frame(self, w, h, bpp):
        with self.lock:
            a = self.active
            if a and not a.get('ended') and a.get('status') == 'running':
                if a['case'] == 1 and (w, h, bpp) == (480, 360, 0):
                    a['frames'] += 1

    def record(self, q, latest):
        self.ready(latest)
        a = self.active
        if not a or a.get('ended') or a.get('status') != 'running':
            raise ValueError('请先选择模式并等待板端确认')
        if (not self.matches(self.board, a['case'], bool(a.get('expected_isp', True)),
                             bool(a.get('expected_jpeg', True))) or
                self.board.get('rid') != a['request_id']):
            raise ValueError('板端确认与当前测试不一致')
        if time.monotonic()-a['confirmed_at'] < a['settle_s']+a['window_s']:
            raise ValueError('请等待稳定5秒并完成观察倒计时后记录电表读数')
        voltage, current = float(q['voltage_v']), float(q['current_ma'])
        if not all(math.isfinite(n) for n in (voltage, current)) or voltage <= 0 or current < 0:
            raise ValueError('电压必须为正数，电流为非负数，均须为有限值')
        if not math.isfinite(voltage*current):
            raise ValueError('输入功率超出数值范围')
        if a['case'] == 1 and not a['frames']:
            raise ValueError('完整运行模式尚未收到有效480×360无线图像，请先检查图传')
        stats = latest.snapshot_stats()
        link_errors = sum(max(0, stats.get(k, 0)-a['baseline'].get(k, 0))
                          for k in ('bad_crc', 'radio_incomplete'))
        row = dict(id=uuid.uuid4().hex, case=a['case'], name=a['name'], session_id=a['id'],
                   recorded_utc=datetime.now(timezone.utc).isoformat(), voltage_v=voltage,
                   current_ma=current, power_mw=voltage*current, input='manual_meter',
                   note=str(q.get('note', a['note']))[:1000], frames=a['frames'],
                   settings=dict(a['settings'],isp=bool(self.board['isp']),jpeg=bool(self.board['jpeg'])),
                   observation_s=a['window_s'],
                   health_errors=a['health_errors'], link_errors=link_errors,
                   comparable=(a['health_errors'] == 0 and link_errors == 0 and
                               self.board.get('skipped', 0) == 0),
                   board=copy.deepcopy(self.board))
        # Adjacent whole-system mode differences, never component-only readings.
        if a['case'] > 1 and row['comparable']:
            reference = next((r for r in reversed(self.readings)
                              if r['case'] == a['case']-1 and r.get('comparable') and
                              abs(r['voltage_v']-voltage) <= 0.01 and
                              r.get('observation_s') == row['observation_s']), None)
            if reference:
                row.update(reference_id=reference['id'],
                           delta_current_ma=reference['current_ma']-current,
                           delta_power_mw=reference['power_mw']-row['power_mw'])
        self.readings.append(row)
        a['readings'].append(copy.deepcopy(row))
        self.event('manual_meter_record', reading_id=row['id'])

    def action(self, q, latest):
        with self.lock:
            action = q.get('action', 'start')
            if action == 'start':
                return self.start(q, latest)
            if action == 'query':
                error = latest.send_cmd('H0\n')
                if error:
                    raise ValueError(error)
            elif action in ('jpeg', 'isp'):
                self.ready(latest)
                enabled = q.get('enabled')
                if not isinstance(enabled, bool):
                    raise ValueError('开关值必须是布尔值')
                self.request_id = self.request_id % 999999 + 1
                expected_isp = self.board.get('isp', 1)
                expected_jpeg = self.board.get('jpeg', 1)
                if action == 'isp': expected_isp = int(enabled)
                else: expected_jpeg = int(enabled)
                self.active = dict(schema='four_mode_v1', id=datetime.now().strftime('%Y%m%d_%H%M%S_')+
                                   uuid.uuid4().hex[:6], case=self.board.get('case', 1),
                                   expected_case=self.board.get('case', 1), status='pending',
                                   expected_isp=expected_isp, expected_jpeg=expected_jpeg,
                                   request_id=self.request_id, ack_deadline=time.monotonic()+10,
                                   _t0=time.monotonic(), events=[], readings=[], frames=0,
                                   window_s=int(q.get('window_s', 30)), settle_s=5,
                                   settings=dict(width=480,height=360,jpeg_qs=12,interval_ms=1000,
                                                 led=0,rf_control=True),note='')
                letter = 'J' if action == 'jpeg' else 'V'
                error = latest.send_cmd('A%d\n%s%d\nH0\n' %
                                        (self.request_id, letter, int(enabled)))
                if error:
                    self.active.update(status='failed', ended=True, error=error)
                    self.save()
                    raise ValueError(error)
            elif action == 'record':
                self.record(q, latest)
            elif action == 'new_group':
                self.readings = []  # Per-mode JSON history remains on disk.
            elif action == 'stop':
                self.ready(latest)
                self.request_id = self.request_id % 999999 + 1
                if not self.active or self.active.get('ended'):
                    self.active = dict(id=datetime.now().strftime('%Y%m%d_%H%M%S_')+uuid.uuid4().hex[:6],
                                       schema='four_mode_v1', _t0=time.monotonic(), case=0,
                                       expected_case=0, events=[], readings=[], frames=0)
                self.active.update(status='stopping', request_id=self.request_id,
                                   ack_deadline=time.monotonic()+10)
                error = latest.send_cmd('A%d\nE0\nH0\n' % self.request_id)
                self.event('pause_command', error=error)
                if error:
                    self.active.update(status='stop_failed', error=error)
                    self.save()
                    raise ValueError('暂停失败，板端状态未确认：'+error)
            else:
                raise ValueError('未知操作')
            self.save()
            return self.state()

    def state(self):
        with self.lock:
            a = self.active
            if a and a.get('status') in ('pending', 'stopping') and time.monotonic() > a['ack_deadline']:
                a['status'] = 'unconfirmed' if a['status'] == 'pending' else 'stop_failed'
                a['error'] = '10秒内未收到匹配板端确认，请查询状态或重新选择模式'
                self.save()
            result = copy.deepcopy(a)
            if result:
                result.pop('_t0', None)
                result['events'] = result.get('events', [])[-30:]
                if result.get('status') == 'running':
                    elapsed = max(0, time.monotonic()-result.pop('confirmed_at'))
                    result['settle_remaining_s'] = max(0, math.ceil(result['settle_s']-elapsed))
                    result['observe_remaining_s'] = max(0, math.ceil(result['settle_s']+result['window_s']-elapsed))
            age = time.time()-self.board.get('received_at', 0)
            return dict(ok=True, schema='four_mode_v1', board=dict(self.board), tx=dict(self.tx),
                        active=result, readings=copy.deepcopy(self.readings),
                        control_ready=(self.board.get('proto') == 6 and age <= 15),
                        board_age_s=round(age, 1) if self.board else None)


experiments = Experiments()
PAGE = Path(__file__).with_name('test_console.html').read_text(encoding='utf-8')
