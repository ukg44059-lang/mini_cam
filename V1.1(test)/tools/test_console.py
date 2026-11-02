"""Experiment sessions, instrument samples and browser controls (no serial owner)."""
import copy
import json
import math
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

CASES = {
    1: '上电初始化（5次）', 2: '系统等待', 3: '只采集图像',
    4: '连续无线图传', 5: '加照明', 6: '接收机关闭',
    7: '间歇拍照', 8: '接收端持续工作',
}
OUT = Path(__file__).resolve().parents[1] / 'test_results'


class Experiments:
    def __init__(self):
        self.lock = threading.RLock()
        self.active = None
        self.history = []
        self.boots = []
        self.telemetry_tail = b''
        self.last_save = 0

    def save(self):
        OUT.mkdir(exist_ok=True)
        if self.active:
            path = OUT / (self.active['id'] + '.json')
            temp = path.with_suffix('.tmp')
            temp.write_text(json.dumps(self.active, ensure_ascii=False, indent=2), encoding='utf-8')
            temp.replace(path)

    def event(self, kind, **data):
        if self.active and not self.active.get('ended'):
            self.active['events'].append(dict(t_s=round(time.monotonic()-self.active['_t0'], 6),
                                               kind=kind, **data))
            if len(self.active['events']) > 10000:
                self.active['events'].pop(0)
                self.active['events_truncated'] = True

    def start(self, q, latest):
        case = int(q['case'])
        if case not in CASES:
            raise ValueError('测试编号必须为1至8')
        if latest.receiver['source'] != 'rf433':
            raise ValueError('请先选择433 MHz无线接收来源')
        if getattr(latest, 'firmware_mode', 'unknown') == 'stream':
            raise ValueError('检测到原图传固件；请先烧录测试版STM32固件，再运行8项测试。当前可正常查看无线图像。')
        duty, interval = int(q.get('duty', 50)), int(q.get('interval', 10))
        if not 0 <= duty <= 100 or duty % 5 or not 1 <= interval <= 3600:
            raise ValueError('占空比为0至100的5%档位；周期1至3600秒')
        phase = q.get('phase', 'wait')
        if phase not in ('wait', 'active'):
            raise ValueError('接收状态无效')
        with self.lock:
            if self.active and not self.active.get('ended'):
                raise ValueError('请先停止当前测试')
            if case == 1 and len(self.boots) >= 5:
                raise ValueError('已记录5次；请先重置上电组')
            # Case 1 must arm before physical power-on, even when UART is absent.
            if case != 1:
                commands = 'I%d\nE%d\n' % (interval, 4 if case == 8 and phase == 'active' else case)
                if case == 5:
                    commands += 'L%d\n' % duty
                error = latest.send_cmd(commands)
                if error:
                    raise ValueError(error)
            self.active = {
                'id': datetime.now().strftime('%Y%m%d_%H%M%S_') + uuid.uuid4().hex[:6],
                'case': case, 'name': CASES[case], 'phase': phase,
                'started_utc': datetime.now(timezone.utc).isoformat(), '_t0': time.monotonic(),
                'settings': dict(width=480, height=360, jpeg_qs=12, rf_mhz=433.92,
                                 rate_kbps=500, tx_power_register='0x913F',
                                 led_duty=duty if case == 5 else 0, interval_s=interval,
                                 pipeline=False, uart_mirror=False),
                'baseline': latest.snapshot_stats(), 'events': [], 'samples': {},
                'energy_j': {}, 'frames': 0, 'first_image_s': None,
                'note': str(q.get('note', ''))[:1000],
                'clock_note': 't_s uses host monotonic time; boot latency requires physical power marker',
            }
            self.event('armed' if case == 1 else 'command_sent')
            self.save()
            return self.state()

    def telemetry(self, raw):
        with self.lock:
            data = self.telemetry_tail + raw
            lines = data.split(b'\n')
            self.telemetry_tail = lines.pop()[-2048:]
            for line in lines:
                if b'[TEST]' in line or b'[RF433' in line:
                    self.event('board', text=line.decode('utf-8', 'replace').strip())
            if self.active and not self.active.get('ended') and time.monotonic()-self.last_save > 5:
                self.save()
                self.last_save = time.monotonic()

    def frame(self, w, h, bpp):
        with self.lock:
            a = self.active
            if not a or a.get('ended'):
                return
            if a['case'] == 1 and 'power_on_s' not in a:
                return
            a['frames'] += 1
            if (w, h, bpp) != (480, 360, 0):
                self.event('unexpected_image', width=w, height=h, format=bpp)
                return
            if a['first_image_s'] is None:
                a['first_image_s'] = time.monotonic() - a['_t0']
                if a['case'] == 1:
                    marker = a.get('power_on_s')
                    a['power_to_image_s'] = a['first_image_s']-marker if marker is not None else None
                self.event('first_valid_wireless_image')
                if a['case'] == 1:
                    a['ended'] = True
                    self.boots.append(copy.deepcopy(a))
                self.save()

    def action(self, q, latest):
        action = q.get('action', 'start')
        if action == 'start':
            return self.start(q, latest)
        with self.lock:
            if action == 'reset_boots':
                if self.active and not self.active.get('ended'):
                    raise ValueError('先停止当前测试')
                self.boots.clear()
            elif action == 'power_on':
                if not self.active or self.active['case'] != 1 or self.active.get('ended'):
                    raise ValueError('先开始上电测试，并在断电状态等待')
                if 'power_on_s' in self.active:
                    raise ValueError('本次上电时刻已标记')
                self.active['power_on_s'] = time.monotonic()-self.active['_t0']
                self.event('physical_power_on_manual_marker')
            elif action == 'stop':
                error = latest.send_cmd('E0\n')
                self.event('stop_command', error=error)
                if self.active:
                    self.active['ended'] = True
                    self.active['final_stats'] = latest.snapshot_stats()
            elif action == 'samples':
                self.add_samples(q)
            else:
                raise ValueError('未知操作')
            self.save()
            return self.state()

    def add_samples(self, q):
        if not self.active:
            raise ValueError('尚无测试记录')
        channel = q.get('channel', 'system')
        if channel not in ('system', 'camera', 'mcu', 'tx', 'rx'):
            raise ValueError('测量通道无效')
        rows = q.get('samples')
        if not isinstance(rows, list) or not rows or len(rows) > 100000:
            raise ValueError('每次提交1至100000个采样点')
        samples = []
        for row in rows:
            t, v, i = (float(row[k]) for k in ('t_s', 'voltage_v', 'current_a'))
            if not all(math.isfinite(n) for n in (t, v, i)) or t < 0 or v < 0:
                raise ValueError('采样值必须有限，时间和电压非负')
            if samples and t <= samples[-1][0]:
                raise ValueError('采样时间必须严格递增')
            samples.append([t, v, i])
        energy = sum((b[0]-a[0])*(a[1]*a[2]+b[1]*b[2])/2
                     for a, b in zip(samples, samples[1:]))
        self.active['samples'][channel] = samples
        self.active['energy_j'][channel] = energy
        self.event('instrument_import', channel=channel, points=len(samples),
                   duration_s=samples[-1][0]-samples[0][0], energy_j=energy)

    def state(self):
        with self.lock:
            a = copy.deepcopy(self.active)
            if a:
                a.pop('_t0', None)
                a['events'] = a['events'][-30:]
                a['samples'] = {k: len(v) for k, v in a['samples'].items()}
            return dict(ok=True, active=a, boot_count=len(self.boots),
                        boot_results=[dict(id=b['id'], power_to_image_s=b.get('power_to_image_s'),
                                           armed_to_image_s=b['first_image_s']) for b in self.boots])


experiments = Experiments()
PAGE = Path(__file__).with_name('test_console.html').read_text(encoding='utf-8')
