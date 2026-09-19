import json
from pathlib import Path
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import serial
from serial.tools import list_ports
from display_perf import verify_identity, WATCH_SERIAL
from watchctl import execute

verify_identity('COM3', WATCH_SERIAL, list_ports.comports())
p = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
p.dtr = p.rts = False
p.port = 'COM3'
p.open()
with Path(sys.argv[1]).open('w', encoding='utf-8') as f:
    def save(line):
        f.write(json.dumps({'t': time.monotonic(), 'line': line}) + '\n')
        f.flush()
    def run(cmd):
        save('> ' + cmd)
        lines = execute(p, cmd, 15)
        for line in lines:
            save(line)
        if any(x.startswith(('ERR ', '? ')) for x in lines):
            raise RuntimeError(lines)
    def observe(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            line = p.readline().decode('utf-8', errors='replace').strip()
            if line:
                save(line)
    try:
        run('awake 1')
        run('perf display info')
        for edge in ('left', 'right'):
            run('home')
            observe(.8)
            save('WORKLOAD ' + edge + ': 12s including overlay entry and 10 alternating swipes')
            run('perf display app current 12')
            run(edge)
            observe(1.0)
            for i in range(10):
                if edge == 'left':
                    cmd = 'swipe 140 130 320 130' if i % 2 == 0 else 'swipe 320 130 140 130'
                else:
                    cmd = 'swipe 233 330 233 130' if i % 2 == 0 else 'swipe 233 130 233 330'
                run(cmd)
                observe(.6)
            observe(5)
    finally:
        for cmd in ('prof 0', 'awake 0', 'resume', 'home'):
            run(cmd)
        p.close()
