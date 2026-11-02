"""Explicit opt-in live checks through receiver only; no physical power cycling."""
import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import json
from pathlib import Path
import time
import urllib.request
import serial


def run():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run', action='store_true', required=True,
                    help='Actually switches camera modes and briefly sets LED to 20 percent')
    ap.add_argument('--url', default='http://127.0.0.1:8081')
    ap.add_argument('--hold-unused-ports', nargs='*', default=['COM30', 'COM34'])
    args = ap.parse_args()
    result = dict(started_utc=datetime.now(timezone.utc).isoformat(), checks=[],
                  physical_power_cycles_tested=False, receiver_power_off_tested=False,
                  current_or_energy_measured=False)

    def get(path):
        with urllib.request.urlopen(args.url+path, timeout=4) as r:
            return json.load(r)

    def action(**data):
        req = urllib.request.Request(args.url+'/test/action', json.dumps(data).encode(),
                                     {'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=4) as r:
            return json.load(r)

    def wait_status(wanted):
        deadline = time.monotonic()+12
        while time.monotonic() < deadline:
            state = get('/test/state')
            if state['active'] and state['active']['status'] == wanted:
                return state
            time.sleep(0.15)
        raise AssertionError('Missing '+wanted+' acknowledgement: '+json.dumps(state))

    with ExitStack() as ports:
        for name in args.hold_unused_ports:
            ports.enter_context(serial.Serial(name, 115200, timeout=0.1))
        result['unused_sender_ports_exclusively_held'] = args.hold_unused_ports
        initial = get('/stats')
        assert initial['receiver']['source'] == 'rf433'
        assert initial['receiver']['control_port'] == initial['receiver']['port']
        result['receiver'] = initial['receiver']
        try:
            action(action='stop');wait_status('stopped')
            for case, phase, duration, image_expected in [
                (2, 'wait', 2, False), (3, 'wait', 2, False),
                (4, 'wait', 6, True), (5, 'wait', 3, True),
                (7, 'wait', 6, True), (8, 'wait', 2, False), (8, 'active', 3, True),
            ]:
                action(action='query')
                action(action='start', case=case, phase=phase, duty=20, interval=2,
                       note='Wireless-only automated functional check; no power measurement')
                state = wait_status('running')
                before = get('/stats')
                time.sleep(duration)
                after = get('/stats')
                count = after['frames']-before['frames']
                assert (count > 0) if image_expected else (count == 0), (case, phase, count)
                assert after['bad_crc'] == before['bad_crc']
                check = dict(case=case, phase=phase, observed_s=duration, new_images=count,
                             board=state['board'], bad_crc=after['bad_crc'],
                             radio_incomplete=after['radio_incomplete'])
                result['checks'].append(check)
                print(json.dumps(check), flush=True)
                action(action='stop');wait_status('stopped')
            result['ok'] = True
        finally:
            action(action='stop')
            state = wait_status('stopped')
            result['final_board'] = state['board']
            result['final_stats'] = get('/stats')
            out = Path(__file__).resolve().parents[1]/'validation/wireless_hardware_check.json'
            out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Wireless-only hardware checks PASS', flush=True)


if __name__ == '__main__':
    run()
