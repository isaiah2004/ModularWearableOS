"""Repeatable USB animation benchmark. Leaves care values intact; restores routine."""
import argparse
import json
from pathlib import Path
import re
import statistics
import time
import serial
from serial.tools import list_ports
from watchctl import execute


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--seconds', type=float, default=8)
    ap.add_argument('--telemetry', action='store_true', help='Enable prof on firmware with opt-in telemetry')
    args = ap.parse_args()
    assert any(p.device == args.port and (p.serial_number or '').lower() == '28:84:85:b0:8b:30'
               for p in list_ports.comports()), 'Confirmed watch identity required'
    port = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
    port.dtr = port.rts = False
    port.port = args.port
    port.open()
    data = {'states': {}, 'commands': []}
    routine = None

    def run(command):
        result = execute(port, command, 15)
        data['commands'].append({'command': command, 'response': result})
        assert not any(s.startswith(('ERR ', '? ')) for s in result), result
        return '\n'.join(result)

    try:
        port.reset_input_buffer()
        run('awake 1')
        if args.telemetry:
            run('prof 1')
        run('board')
        run('face 1')
        routine = re.search(r'routine=(\d+)-(\d+)', run('pip')).groups()
        run('pip routine 0 23')
        run('pip center')
        for action in ('idle', 'petted', 'grumpy', 'play', 'dizzy'):
            run('pip demo ' + action)
            start = time.monotonic()
            next_force = start + 2
            lines = []
            while time.monotonic() - start < args.seconds + 2:
                if time.monotonic() >= next_force:
                    # Replay before the shortest measured action (2.8 s) ends.
                    run('pip demo ' + action)
                    next_force = time.monotonic() + 2
                line = port.readline().decode('utf-8', errors='replace').strip()
                if time.monotonic() - start >= 2 and 'PROF ' in line:
                    lines.append(line)
            fps = [int(re.search(r'fps=(\d+)', line)[1]) for line in lines]
            render = [float(re.search(r'render=([\d.]+)', line)[1]) for line in lines]
            assert fps, 'No frame telemetry'
            data['states'][action] = {'fps_median': statistics.median(fps),
                'fps_min': min(fps), 'fps_max': max(fps),
                'render_ms_median': statistics.median(render), 'samples': lines}
            for metric in ('record', 'raster', 'present', 'wait', 'p95_ms', 'max_ms'):
                values = [float(m[1]) for line in lines
                          if (m := re.search(r'\b' + metric + r'=([\d.]+)', line))]
                if values:
                    data['states'][action][metric + '_sample_median'] = statistics.median(values)
            print(action, {k: v for k, v in data['states'][action].items() if k != 'samples'}, flush=True)
    finally:
        try:
            if routine:
                run('pip routine ' + ' '.join(routine))
            run('pip live')
            if args.telemetry:
                run('prof 0')
            run('awake 0')
        finally:
            port.close()
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
