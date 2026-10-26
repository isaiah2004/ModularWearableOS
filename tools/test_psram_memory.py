"""Run upstream heavy_reopen memory assertions while explicitly omitting legacy timing gates.

The upstream test remains unchanged. The generated script expands every include,
logs each omitted `expect nolate` with its source line, and retains all heap,
fault, lifecycle, idle and blur-coverage assertions. Simulator model timings are
still reported, but this diagnostic is not a timing or device-FPS acceptance test.
"""
import argparse
import ast
from pathlib import Path
import re
import subprocess
from test_settings_blur import check_cold_fallback

ROOT = Path(__file__).resolve().parents[1]


def heavy_lines():
    """Evaluate upstream setup up to its first case, avoiding copied workload drift."""
    path = ROOT / "tools/test_psram.py"
    tree = ast.parse(path.read_text(), filename=str(path))
    setup = []
    for node in tree.body:
        if (isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Name) and node.value.func.id == "run"
                and node.value.args and isinstance(node.value.args[0], ast.Constant)
                and node.value.args[0].value == "heavy_reopen"):
            scope = {"__file__": str(path), "__name__": "memory_workload_setup"}
            exec(compile(ast.Module(body=setup, type_ignores=[]), str(path), "exec"), scope)
            return scope["lines"]
        setup.append(node)
    raise RuntimeError("Upstream heavy_reopen case was not found; update this diagnostic")


def expand(lines, source, excluded, ancestry=()):
    for number, line in enumerate(lines, 1):
        command = line.split("#", 1)[0].strip()
        if command.startswith("include "):
            path = (ROOT / command[len("include "):].strip()).resolve()
            if path in ancestry:
                raise RuntimeError(f"Recursive script include: {path}")
            yield from expand(path.read_text().splitlines(), path.relative_to(ROOT),
                              excluded, (*ancestry, path))
        elif command == "expect nolate":
            excluded.append(f"{source}:{number}: {line.strip()}")
            yield f"# Memory-only diagnostic excluded {source}:{number}: {line.strip()}"
        else:
            yield line


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, default=ROOT / "build/nos_sim.exe")
    parser.add_argument("--out", type=Path, default=ROOT / "build/psram_memory")
    parser.add_argument("--temporal-fallback", action="store_true",
                        help="explicitly replace old aggregate directional100%% gate with per-fast-frame compositor audit")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    excluded = []
    lines = list(expand(heavy_lines(), "upstream generated heavy_reopen", excluded))
    if args.temporal_fallback:
        lines.insert(0, "motion on")
    script = args.out / "heavy_reopen_memory.txt"
    script.write_text("\n".join(lines) + "\n")
    print(f"EXCLUDED {len(excluded)} legacy timing assertions (memory diagnostic only):", flush=True)
    for item in excluded:
        print(f"  {item}", flush=True)
    print("All non-timing upstream script assertions remain enabled.", flush=True)
    if args.temporal_fallback:
        print("EXPLICIT CONTRACT: cold Settings may use temporal fallback; legacy aggregate directional100% gate reported separately. Each audited fast Settings frame must process its full painted region and have actual temporal blends.", flush=True)
    else:
        print("Legacy aggregate kind1 100% directional coverage gate remains enabled.", flush=True)
    proc = subprocess.run([str(args.sim.resolve()), "--script", str(script.resolve()),
                           "--out", str((args.out / "heavy_reopen").resolve()), "--no-media"],
                          cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = args.out / "heavy_reopen_memory.log"
    log.write_bytes(proc.stdout)
    output = proc.stdout.decode(errors="replace")
    motion = args.out / "heavy_reopen/motion.txt"
    if motion.exists():
        audit = re.search(r"(\d+) moves \(\d+ judged\), (\d+) FAIL", motion.read_text())
        if audit:
            print(f"INDEPENDENT MOTION-SHAPE AUDIT: {audit[2]} failures across {audit[1]} moves; not waived by memory success; see {motion}", flush=True)

    if proc.returncode:
        print(f"FAIL: upstream memory assertions; see {log}", flush=True)
        return 1
    match = re.search(r"BLUR kind=1 moving=\d+ fast=(\d+) blurred=(\d+)", output)
    if match:
        print(f"Directional kind1 coverage: {match[2]}/{match[1]} fast frames", flush=True)
    if args.temporal_fallback:
        count = check_cold_fallback(args.out / "heavy_reopen")
        print(f"PASS compositor audit: {count} fast Settings frames fully processed with actual temporal exposure", flush=True)
    elif not match or not int(match[1]) or match[1] != match[2]:
        print(f"FAIL: kind1 directional coverage; see {log}", flush=True)
        return 1
    print(f"PASS: heavy_reopen memory/lifecycle/blur assertions; timing gates excluded; see {log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
