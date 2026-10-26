"""Copy raw IMU segments with incomplete final rows removed; originals stay intact."""
from pathlib import Path
import argparse
import csv
import math

HEADER='ms,utc_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,a_g\n'

def recover_segment(source: Path, destination: Path) -> tuple[int, int]:
    """Return (complete rows, omitted tail bytes). Destination must not exist."""
    rows=0
    with source.open('rb') as src, destination.open('xb') as dst:
        if src.readline(256)!=HEADER.encode():
            raise ValueError(f'{source}: unsupported CSV header')
        dst.write(HEADER.encode())
        while True:
            line=src.readline(512)
            if not line: return rows,0
            if not line.endswith(b'\n'):
                tail=len(line)
                for block in iter(lambda:src.read(4096),b''): tail+=len(block)
                return rows,tail
            fields=next(csv.reader([line.decode('ascii')]))
            if len(fields)!=9 or any(not math.isfinite(float(v)) for v in fields):
                raise ValueError(f'{source}: malformed complete row {rows+1}')
            int(fields[0]);int(fields[1])
            dst.write(line); rows+=1

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path,help='downloaded /imu directory')
    parser.add_argument('output',type=Path,help='new directory for recovered CSV copies')
    args=parser.parse_args()
    if args.input.resolve()==args.output.resolve(): parser.error('output must differ from originals')
    args.output.mkdir(parents=True,exist_ok=True)
    for source in sorted(args.input.glob('[0-9][0-9][0-9][0-9].csv')):
        rows,tail=recover_segment(source,args.output/source.name)
        print(f'{source.name}: {rows} complete rows, {tail} incomplete tail bytes omitted')
        link=source.with_suffix('.link')
        if link.is_file():
            with (args.output/link.name).open('xb') as out: out.write(link.read_bytes())

if __name__=='__main__': main()
