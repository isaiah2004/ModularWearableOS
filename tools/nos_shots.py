#!/usr/bin/env python3
"""nos_shots.py — review helpers for NOS simulator runs (docs/NOS_SIM.md).

  python tools/nos_shots.py run SCRIPT [OUT]      build/nos_sim.exe --script, then `report`
  python tools/nos_shots.py report DIR            DIR/index.html: shots, films, gifs, cost table
  python tools/nos_shots.py diff OLD NEW          compare two runs: changed shots + cost deltas
                                                   (writes NEW/diff_*.png for changed shots)
  python tools/nos_shots.py late DIR [N]          the N most expensive frames from frames.csv

Pure standard library except `diff`, which uses Pillow when available.
"""
import csv, html, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "build", "nos_sim.exe")


def read_frames(d):
    p = os.path.join(d, "frames.csv")
    if not os.path.exists(p):
        return []
    with open(p, newline="") as f:
        return list(csv.DictReader(f))


def pct(vals, p):
    if not vals:
        return 0.0
    v = sorted(vals)
    i = max(0, min(len(v) - 1, int(-(-p * len(v) // 100)) - 1))
    return v[i]


def sections(rows):
    out = {}
    for r in rows:
        out.setdefault(r["section"], []).append(r)
    return out


def cost_table(rows):
    lines = []
    for name, rs in sections(rows).items():
        ms = [float(r["est_frame_ms"]) for r in rs]
        sent = [float(r["est_frame_ms"]) for r in rs if int(r["dirty_px"]) > 0]
        late = sum(int(r["late"]) for r in rs)
        lines.append((name, len(rs), len(sent), pct(ms, 50), pct(sent, 95), max(ms) if ms else 0, late))
    return lines


def cmd_run(script, out=None):
    args = [SIM, "--script", script]
    if out:
        args += ["--out", out]
    else:
        out = os.path.join(ROOT, "build", "nos_out", os.path.splitext(os.path.basename(script))[0])
    rc = subprocess.call(args, cwd=ROOT)
    cmd_report(out)
    return rc


def cmd_report(d):
    rows = read_frames(d)
    files = sorted(os.listdir(d))
    pngs = [f for f in files if f.endswith(".png") and not f.startswith("diff_")]
    gifs = [f for f in files if f.endswith(".gif")]
    h = ["<!doctype html><meta charset=utf-8><title>NOS run %s</title>" % html.escape(d),
         "<style>body{background:#0b0b0d;color:#ddd;font:13px Consolas,monospace;margin:20px}"
         "img{max-width:100%;background:#000;border-radius:6px}figure{display:inline-block;margin:8px;vertical-align:top}"
         "figcaption{color:#999;margin-top:4px}table{border-collapse:collapse}td,th{padding:3px 10px;border-bottom:1px solid #222}"
         ".late{color:#ff6a50}</style>",
         "<h2>%s</h2>" % html.escape(d)]
    summ = os.path.join(d, "summary.txt")
    if os.path.exists(summ):
        h.append("<pre>%s</pre>" % html.escape(open(summ).read()))
    if rows:
        h.append("<table><tr><th>section</th><th>frames</th><th>sent</th><th>p50 ms</th><th>p95 sent</th><th>max</th><th>late</th></tr>")
        for name, n, ns, p50, p95, mx, late in cost_table(rows):
            h.append("<tr><td>%s</td><td>%d</td><td>%d</td><td>%.1f</td><td>%.1f</td><td>%.1f</td><td class=%s>%d</td></tr>"
                     % (html.escape(name), n, ns, p50, p95, mx, "late" if late else "", late))
        h.append("</table>")
    for g in gifs:
        h.append("<figure><img src='%s' width=466><figcaption>%s</figcaption></figure>" % (g, g))
    for p in pngs:
        w = 466 if not p.endswith("_000.png") else 233
        h.append("<figure><img src='%s' style='max-width:%dpx'><figcaption>%s</figcaption></figure>" % (p, max(w, 466), p))
    out = os.path.join(d, "index.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(h))
    print("wrote", out)


def cmd_diff(old, new):
    ro, rn = read_frames(old), read_frames(new)
    if ro and rn:
        to = {t[0]: t for t in cost_table(ro)}
        print("%-16s %10s %10s %8s" % ("section", "p95 old", "p95 new", "late"))
        for t in cost_table(rn):
            o = to.get(t[0])
            print("%-16s %10s %10.1f %4s->%d" % (t[0], "%.1f" % o[4] if o else "-", t[4], o[6] if o else "-", t[6]))
    try:
        from PIL import Image, ImageChops
    except ImportError:
        print("(Pillow not installed: skipping image comparison)")
        return 0
    changed = 0
    for f in sorted(os.listdir(new)):
        if not f.endswith(".png") or f.startswith("diff_"):
            continue
        po = os.path.join(old, f)
        if not os.path.exists(po):
            print("new shot   ", f)
            continue
        a, b = Image.open(po).convert("RGB"), Image.open(os.path.join(new, f)).convert("RGB")
        if a.size != b.size:
            print("size change", f, a.size, "->", b.size)
            changed += 1
            continue
        d = ImageChops.difference(a, b)
        bbox = d.getbbox()
        if bbox:
            n = sum(1 for px in d.getdata() if px != (0, 0, 0))
            print("changed    %-28s %7d px  bbox %s" % (f, n, bbox))
            mask = d.convert("L").point(lambda v: 255 if v else 0)
            hl = Image.composite(Image.new("RGB", b.size, (255, 0, 200)), b.point(lambda v: v // 3), mask)
            sheet = Image.new("RGB", (b.width * 3 + 16, b.height), (8, 8, 10))
            sheet.paste(a, (0, 0)); sheet.paste(b, (b.width + 8, 0)); sheet.paste(hl, (b.width * 2 + 16, 0))
            sheet.save(os.path.join(new, "diff_" + f))
            changed += 1
    print("%d shot(s) changed" % changed)
    return 0


def cmd_late(d, n=15):
    rows = read_frames(d)
    rows.sort(key=lambda r: -float(r["est_frame_ms"]))
    cls = [k for k in (rows[0].keys() if rows else []) if k.startswith("ms_")]
    for r in rows[:n]:
        top = sorted(cls, key=lambda k: -float(r[k]))[:3]
        print("f%-6s %5.1f ms  raster %5.1f bus %5.1f  dirty %7s  %-12s %-14s %s" % (
            r["frame"], float(r["est_frame_ms"]), float(r["est_raster_ms"]), float(r["est_bus_ms"]),
            r["dirty_px"], r["section"], r["screen"],
            " ".join("%s=%.1f" % (k[3:], float(r[k])) for k in top if float(r[k]) > 0)))


def main(a):
    if len(a) >= 2 and a[0] == "run":
        return cmd_run(a[1], a[2] if len(a) > 2 else None)
    if len(a) == 2 and a[0] == "report":
        return cmd_report(a[1])
    if len(a) == 3 and a[0] == "diff":
        return cmd_diff(a[1], a[2])
    if len(a) >= 2 and a[0] == "late":
        return cmd_late(a[1], int(a[2]) if len(a) > 2 else 15)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]) or 0)
