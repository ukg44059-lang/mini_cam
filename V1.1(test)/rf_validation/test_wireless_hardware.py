"""Explicit live four-mode check using only the receiver UART. No meter values."""
import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import json
from pathlib import Path
import time
import urllib.request
import serial
import serial.tools.list_ports


def run():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run', action='store_true', required=True)
    ap.add_argument('--url', default='http://127.0.0.1:8081')
    ap.add_argument('--hold-unused-ports', nargs='*', default=[])
    args = ap.parse_args()
    result = dict(schema='four_mode_v1', started_utc=datetime.now(timezone.utc).isoformat(),
                  checks=[], current_or_energy_measured=False, led_tested=False)

    def get(path):
        with urllib.request.urlopen(args.url+path, timeout=4) as r:
            return json.load(r)

    def action(**data):
        req = urllib.request.Request(args.url+'/test/action', json.dumps(data).encode(),
                                     {'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=4) as r:
            return json.load(r)

    def wait_status(wanted):
        deadline = time.monotonic()+13
        while time.monotonic() < deadline:
            state = get('/test/state')
            if state['active'] and state['active']['status'] == wanted:
                return state
            time.sleep(0.2)
        raise AssertionError('Missing '+wanted+' acknowledgement: '+json.dumps(state))

    with ExitStack() as ports:
        for name in args.hold_unused_ports:
            ports.enter_context(serial.Serial(name, 115200, timeout=0.1))
        result['unused_sender_ports_exclusively_held'] = args.hold_unused_ports
        result['visible_ports'] = [p.device for p in serial.tools.list_ports.comports()]
        initial = get('/stats')
        assert initial['receiver']['source'] == 'rf433'
        assert initial['receiver']['control_port'] == initial['receiver']['port']
        result['receiver'] = initial['receiver']
        try:
            # Verify recovery to full image transfer from each reduced mode twice.
            for case in [1, 2, 1, 3, 1, 4, 1, 2, 3, 4, 1]:
                action(action='query')
                action(action='start', case=case, note='Automated functional check; no electrical measurement')
                state = wait_status('running')
                expected = {1:(0,0,1),2:(0,0,0),3:(1,0,0),4:(1,1,0)}[case]
                board = state['board']
                assert board['proto'] == 6 and board['fault'] == 0 and board['led'] == 0
                assert tuple(board[k] for k in ('standby','sleep','image')) == expected
                # Drain any image already en route before taking the quiet-mode baseline.
                time.sleep(1)
                action(action='query')
                time.sleep(0.5)
                before_board = get('/test/state')['board']
                before = get('/stats')
                time.sleep(4)
                action(action='query')
                time.sleep(0.5)
                after_state = get('/test/state')
                after_board = after_state['board']
                after = get('/stats')
                count = after['frames']-before['frames']
                assert (count > 0) if case == 1 else count == 0, (case, count)
                assert after['bad_crc'] == before['bad_crc']
                assert after['radio_incomplete'] == before['radio_incomplete']
                assert after_state['active']['status'] == 'running'
                assert after_board['rid'] == state['active']['request_id']
                delta = after_board['captures']-before_board['captures']
                assert 3 <= delta <= 6 if case in (1,2) else delta == 0, (case, delta)
                assert after_board['skipped'] == 0, after_board
                check = dict(case=case, observed_s=4.5, new_wireless_images=count,
                             capture_delta=delta, board=after_board,
                             new_crc_errors=after['bad_crc']-before['bad_crc'],
                             new_incomplete=after['radio_incomplete']-before['radio_incomplete'])
                result['checks'].append(check)
                print(json.dumps(check), flush=True)
            action(action='stop')
            stopped = wait_status('stopped')
            assert stopped['board']['standby'] == 1 and stopped['board']['sleep'] == 1
            result['pause_confirmed'] = True
        finally:
            # Leave the user's image pipeline running at the end of validation.
            try:
                action(action='start', case=1, note='实板验证结束，恢复完整运行；等待人工电表测量')
                result['restored_full'] = wait_status('running')['board']
            finally:
                result['completed_utc'] = datetime.now(timezone.utc).isoformat()
                result['ok'] = len(result['checks']) == 11 and result.get('pause_confirmed', False)
                output = Path(__file__).resolve().parents[1]/'validation/four_mode_hardware_check.json'
                output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    assert result['ok']


if __name__ == '__main__':
    run()
