"""Check wake settings and display power over USB. Physical gestures require an owner trace."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import time
import serial
from serial.tools import list_ports
from watchctl import execute


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--log', type=Path, default=Path('build/usb-wake-check.jsonl'))
    parser.add_argument('--after-reset', action='store_true')
    args = parser.parse_args()
    assert any(p.device == args.port and (p.serial_number or '').lower() == '28:84:85:b0:8b:30'
               for p in list_ports.comports()), 'Confirmed watch identity required'
    port = serial.Serial(port=None, baudrate=115200, timeout=.15, write_timeout=2)
    port.dtr = port.rts = False
    port.port = args.port
    port.open()
    args.log.parent.mkdir(parents=True, exist_ok=True)

    def run(command, delay=.15, error=False):
        lines = execute(port, command, 15)
        with args.log.open('a', encoding='utf-8') as f:
            f.write(json.dumps({'time': datetime.now().astimezone().isoformat(),
                                'command': command, 'response': lines}) + '\n')
        errors = [line for line in lines if line.startswith(('ERR ', '? '))]
        assert bool(errors) == error, (command, lines)
        print(command + ' -> ' + ' | '.join(lines), flush=True)
        time.sleep(delay)
        return dict(re.findall(r'(\w+)=([^\s]+)', '\n'.join(lines)))

    def check(**expected):
        actual = run('wake')
        for key, value in expected.items():
            assert actual[key] == str(value), (key, actual, expected)
        return actual

    try:
        time.sleep(.25)
        port.reset_input_buffer()
        board = run('board')
        assert board['flash'] == '16MB' and board['imu'] == '1' and board['touch'] == '1'
        run('info');run('time');run('pip')
        if args.after_reset:
            check(wrist='right', pickup=0, turn=1, valid=1)
            run('wake left');run('wake pickup 1');run('wake turn 1')
            check(wrist='left', pickup=1, turn=1)
            run('face 1');run('awake 0')
            print('PASS hardware: wake preferences survived reboot; left wrist and both gestures restored')
            return
        run('awake 1')
        run('wake left');run('wake pickup 1');run('wake turn 1')
        check(wrist='left', pickup=1, turn=1, valid=1)
        run('wake pickup 2', error=True);check(pickup=1)
        run('open 23', .8);run('tap 233 291', .4)
        run('tap 299 148');check(wrist='right')
        run('tap 167 148');check(wrist='left')
        run('tap 345 217');check(pickup=0, turn=1)
        run('tap 345 217');check(pickup=1, turn=1)
        run('tap 345 283');check(pickup=1, turn=0)
        run('tap 345 283');check(pickup=1, turn=1)
        run('diag imu', .8)
        assert run('info')['app'].startswith('16/')
        run('imu');run('face 1', .8)
        for mode in ['off', 'dim']:
            run('screen ' + mode, .3)
            for _ in range(4):
                check(valid=1)
                assert run('screen', .3)['state'] == mode, 'Keep watch still during display-off test'
            run('tap 233 200', .4)
            assert run('screen')['state'] == 'awake'
        run('aod', .3)
        assert run('screen')['glance'] == '0'
        run('tap 233 200', .3)
        assert run('screen')['glance'] == '1'
        for _ in range(10):
            run('wake', 1)
        assert run('screen')['glance'] == '0'
        run('aod')
        assert run('screen')['aod'] == '0'
        run('wake right');run('wake pickup 0');run('wake turn 1')
        check(wrist='right', pickup=0, turn=1)
        run('pip save');run('pip');run('time');run('awake 0')
        print('PASS hardware: settings touch targets, independent modes, raw IMU, off/dim query stability, touch wake and timed AOD glance')
        print('Ready for reset test: right wrist, pickup off, wrist-turn on. Run --after-reset to verify and restore left wrist.')
    finally:
        port.close()


if __name__ == '__main__':
    main()
