"""Exercise the installed Pip build; log observations without claiming visual QA."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import time
import serial
from serial.tools import list_ports
from watchctl import execute


def fields(lines):
    return dict(re.findall(r'(\w+)=([^\s]+)', '\n'.join(lines)))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--log', type=Path, default=Path('build/usb-pip-tour.jsonl'))
    args = ap.parse_args()
    assert any(p.device == args.port and (p.serial_number or '').lower() == '28:84:85:b0:8b:30'
               for p in list_ports.comports()), 'Confirmed watch identity required'
    port = serial.Serial(port=None, baudrate=115200, timeout=.15, write_timeout=2)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    original_routine = None

    def run(command, delay=.15):
        result = execute(port, command, 15)
        with args.log.open('a', encoding='utf-8') as log:
            log.write(json.dumps({'time': datetime.now().astimezone().isoformat(),
                                  'command': command, 'response': result}) + '\n')
        assert not any(x.startswith(('ERR ', '? ')) for x in result), (command, result)
        print(command + ' -> ' + next((x for x in result if not x.startswith('I (')), 'DONE'), flush=True)
        time.sleep(delay)
        return result

    def wait_app(index):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            status=fields(run('info', .15))
            if status['app'].split('/')[0]==str(index):
                return
        raise AssertionError(f'App {index} did not finish its transition')

    try:
        time.sleep(.25)
        port.reset_input_buffer()
        run('awake 1')
        board = fields(run('board'))
        assert board['flash'] == '16MB' and board['touch'] == '1' and board['imu'] == '1'
        assert board['rtc_valid'] == '1'
        run('face 1', .8)
        before = fields(run('pip'))
        original_routine = before['routine'].split('-')
        run('pip routine 0 23')  # current local time is just after midnight
        run('pip live', 2)
        power = fields(run('power'))
        assert power['external'] == '1' and power['valid'] == '0' and power['sample'] == '1'
        assert fields(run('activity'))['valid'] == '1'
        baseline = int(fields(run('pip'))['notify_seq'])
        for i in range(12):
            run(f'notify Pip test|Arrival {i + 1:02}', .05)
        states = []
        for _ in range(8):
            status = fields(run('pip', .2))
            states.append(status['state'])
        assert int(status['notify_seq']) == baseline + 12 and status['notifications'] == '8'
        assert 'notify' in states, states
        run('pip pet')
        assert fields(run('pip'))['state'] == 'petted'
        run('pip play')
        assert fields(run('pip'))['state'] == 'play'
        for action in ['grumpy', 'lonely', 'excited', 'relieved', 'notify', 'dizzy', 'asleep', 'wake']:
            run('pip demo ' + action)
            assert fields(run('pip'))['state'] == action
        run('pip care', .7)
        assert 'name=Pet care' in '\n'.join(run('info'))
        run('swipe 310 275 130 275', .7)
        run('swipe 310 275 130 275', .7)
        run('tap 233 420', .7)
        app_count = int(fields(run('info'))['app'].split('/')[1])
        for index in range(app_count):
            run(f'open {index}', .35)
            wait_app(index)
        for face in range(13):
            run(f'face {face}', .8)
            assert 'face=' + str(face) in '\n'.join(run('face'))
        run('face 1', .5)
        for edge in ['left', 'right', 'top', 'bottom']:
            run(edge, .6)
            run('close', .5)
        run('pip center')
        run('pip save')
        assert fields(run('pip'))['save_error'] == '0'
        run('board')
        print(f'PASS device: 12 arrivals, expressions, care, {app_count} apps, 13 faces, 4 edges', flush=True)
    finally:
        try:
            if original_routine:
                run('pip routine ' + ' '.join(original_routine))
            run('face 1')
            run('pip hello')
            run('pip save')
            run('awake 0')
        finally:
            port.close()


if __name__ == '__main__':
    main()
