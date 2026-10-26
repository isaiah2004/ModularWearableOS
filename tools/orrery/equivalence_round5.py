"""Compare damage/full pixels; original scripts/assertions run in check_round5.ps1."""
import csv
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/orrery/round5_equivalence"
OUT.mkdir(parents=True, exist_ok=True)
total = 0
for source in sorted((ROOT / "tools/orrery").glob("*.txt")):
    script = OUT / source.name
    script.write_text("\n".join(line for line in source.read_text().splitlines()
                              if not line.lstrip().startswith("expect ")) + "\n")
    hashes = []
    for mode in ("damage", "full"):
        dest = OUT / f"{source.stem}_{mode}"
        digest = OUT / f"{source.stem}_{mode}.hash"
        args = [str(ROOT / "build/nos_sim_round5.exe"), "--script", str(script),
                "--out", str(dest), "--hashes", str(digest), "--no-media", "--quiet"]
        if mode == "full":
            args.append("--force-full")
        subprocess.run(args, cwd=ROOT, check=True, capture_output=True)
        hashes.append(digest.read_bytes())
    assert hashes[0] == hashes[1], f"damage/full mismatch: {source.name}"
    total += len(hashes[0].splitlines())
print(f"PASS damage/full pixels: {total} frames")

for dest in sorted((ROOT / "build/orrery/round5_test").iterdir()):
    file = dest / "frames.csv"
    if not file.exists() or dest.name in ("unit", "cost", "nocost"):
        continue
    rows = list(csv.DictReader(file.open()))
    late = [r for r in rows if r["late"] == "1"]
    sections = sorted({r["section"] for r in late})
    print(f"{dest.name}: {len(late)}/{len(rows)} late; {', '.join(sections)}")

rows = list(csv.DictReader((ROOT / "build/orrery/round5_test/perf_round5/frames.csv").open()))
for section in dict.fromkeys(r["section"] for r in rows):
    sent = [r for r in rows if r["section"] == section and int(r["dirty_px"]) > 0]
    if sent:
        values = [float(r["est_frame_ms"]) for r in sent]
        print(f"{section}: sending p50={statistics.median(values):.3f} max={max(values):.3f}")
