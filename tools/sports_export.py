"""Stream checked Sports v1 journals to CSV. Never modifies watch data.

python tools/sports_export.py /path/to/sd/watch/sports --out sports.csv
python tools/sports_export.py /path/to/sports --day 20261020 --start 12:00 --end 14:00 --events --out afternoon.csv
"""
import argparse
import csv
import datetime as dt
import pathlib
import struct
import sys

RECORD = struct.Struct('<6I2H8fI')
NAMES = ['?', 'BOOT', 'POWER', 'BATTERY', 'ACTIVITY', 'WALK', 'KNOCK',
         'WAKE', 'GESTURE', 'REPORT', 'LABEL', 'TUNE', 'FOOD', 'WATER',
         'IMU', 'CLIMATE', 'LIGHT', 'COMPASS', 'HEART', 'GPS', 'ROUTINE', 'PPG',
         'RAIL', 'LIGHT RAW', 'MAG RAW', 'MIC', 'TOUCH', 'LOCK']
FIELDS = {
    'POWER': 'mode cpu_mhz brightness',
    'BATTERY': 'level_percent rail_mv external_power battery_valid sample_valid mode age_ms',
    'ACTIVITY': 'steps estimated_active_kcal active_minutes',
    'WALK': 'walking candidates valid steps filtered_g peak_g valley_g',
    'KNOCK': 'reason z_g sideways_g interval_ms gyro_dps',
    'WAKE': 'triggered stage reason view_dot speed learn_result',
    'GESTURE': 'direction', 'REPORT': 'feature outcome lookback_seconds',
    'LABEL': 'feature trial_active expected_repetitions',
    'TUNE': 'feature walk_g knock_g entry_peaks quiet_ms',
    'FOOD': 'kcal', 'WATER': 'ml', 'IMU': 'ax_g ay_g az_g gx_dps gy_dps gz_dps',
    'CLIMATE': 'valid temperature_c humidity_percent pressure_hpa altitude_m',
    'LIGHT': 'valid lux proximity', 'COMPASS': 'valid heading_deg field_ut calibrated progress',
    'HEART': 'on_wrist bpm spo2 quality', 'GPS': 'valid latitude longitude altitude_m speed_mps hdop satellites',
    'ROUTINE': 'routine state result', 'PPG': 'red_adc ir_adc',
    'RAIL': 'level_percent rail_mv external_power battery_valid sample_valid mode',
    'LIGHT RAW': 'clear_adc red_adc green_adc blue_adc', 'MAG RAW': 'x_adc y_adc z_adc',
    'MIC': 'db_estimate peak envelope', 'TOUCH': 'x y down', 'LOCK': 'locked',
}

def checksum(raw):
    h = 2166136261
    for b in raw:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h

def minute(text):
    h, m = map(int, text.split(':'))
    if not (0 <= h <= 24 and 0 <= m < 60 and (h < 24 or m == 0)):
        raise argparse.ArgumentTypeError('use HH:MM from 00:00 to 24:00')
    return h * 60 + m

def records(path):
    with path.open('rb') as src:
        offset = 0
        while raw := src.read(RECORD.size):
            if len(raw) != RECORD.size:
                print(f'{path}:{offset}: incomplete tail ignored', file=sys.stderr)
                break
            row = RECORD.unpack(raw)
            if row[0] != 0x53505431 or checksum(raw[:-4]) != row[-1]:
                print(f'{path}:{offset}: corrupt record ignored', file=sys.stderr)
            else:
                yield row
            offset += RECORD.size

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=pathlib.Path)
    ap.add_argument('--out', required=True, type=pathlib.Path)
    ap.add_argument('--day', type=int)
    ap.add_argument('--start', type=minute, default=0)
    ap.add_argument('--end', type=minute, default=1440)
    ap.add_argument('--events', action='store_true', help='omit raw sensor files')
    args = ap.parse_args()
    if args.end <= args.start:
        ap.error('--end must follow --start')
    paths = [args.source] if args.source.is_file() else sorted(args.source.glob('*-*.spt'))
    if not paths:
        ap.error('no .spt journals found')
    # x mode prevents accidentally replacing an earlier export.
    with args.out.open('x', newline='', encoding='utf-8') as dest:
        writer = csv.writer(dest)
        writer.writerow(['file', 'boot_id', 'sequence', 'uptime_ms', 'utc', 'local_day', 'local_minute', 'type',
                         *[x for i in range(8) for x in (f'field_{i}', f'value_{i}')]])
        for path in paths:
            if args.events and '-sensors.' in path.name:
                continue
            for r in records(path):
                _, boot, seq, uptime, epoch, day, mins, kind, *values = r
                if args.day is not None and day != args.day:
                    continue
                if not args.start <= mins < args.end:
                    continue
                name = NAMES[kind] if kind < len(NAMES) else str(kind)
                fields = FIELDS.get(name, '').split()
                utc = dt.datetime.fromtimestamp(epoch, dt.timezone.utc).isoformat() if epoch else ''
                writer.writerow([path.name, boot, seq, uptime, utc, day, mins, name,
                                 *[x for i in range(8) for x in (fields[i] if i < len(fields) else '', values[i])]])

if __name__ == '__main__':
    main()
