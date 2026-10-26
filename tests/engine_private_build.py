"""Fresh ABI-consistent simulator objects for engine integration regressions.

Outputs are private: never reads or writes build/nos_obj or a shared executable.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ROOT / "build/alpha05/checks/engine"
FLAGS = ["-O2", "-std=c11", "-DNOS_PSRAM_TRACKING", "-DGFX_COST_MODEL",
         "-I" + str(ROOT / "core"), "-I" + str(ROOT / "nos"),
         "-I" + str(ROOT / "nos/fonts"), "-I" + str(ROOT / "sim"),
         "-I" + str(ROOT / "firmware/main"), "-Wall", "-Wextra",
         "-Wno-unused-parameter", "-Wno-format-truncation", "-Werror"]
LIBRARIES = ["-lgdi32", "-luser32", "-lwinmm", "-lm"]


def build_objects(out):
    out = Path(out).resolve()
    assert out.is_relative_to(CHECKS.resolve()), out
    out.mkdir(parents=True, exist_ok=True)
    core = ("asset_job", "gfx", "font", "sprite", "anim", "notif", "battery",
            "pedometer", "walk", "motion_wake", "wake_service", "knock", "ephem")
    sources = sorted((ROOT / "nos").rglob("*.c"))
    sources += [ROOT / "core" / (name + ".c") for name in core]
    sources += sorted((ROOT / "core/fonts").glob("*.c"))
    sources += sorted((ROOT / "sim").glob("nos_*.c"))

    def compile_source(source):
        name = source.relative_to(ROOT).as_posix().replace("/", "_").replace(".c", ".o")
        obj = out / name
        extra = ["-Dmain=sim_program_main"] if source == ROOT / "sim/nos_sim.c" else []
        result = subprocess.run(["gcc", *FLAGS, *extra, "-c", str(source), "-o", str(obj)],
                                cwd=ROOT, capture_output=True, text=True)
        return source, obj, result

    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(compile_source, sources))
    log = "\n".join(f"{src.relative_to(ROOT)}: {result.returncode}\n{result.stdout}{result.stderr}"
                    for src, _, result in results)
    (out / "compile.log").write_text(log)
    (CHECKS / "fresh-build.log").write_text(log)
    failures = [str(src.relative_to(ROOT)) for src, _, result in results if result.returncode]
    if failures:
        raise AssertionError("Fresh engine compilation failed: " + ", ".join(failures) + "\n" + log)
    return {src.relative_to(ROOT).as_posix(): obj for src, obj, _ in results}


def link_test(source, binary, objects, exclude=()):
    result = subprocess.run(["gcc", *FLAGS, str(ROOT / source),
                             *(str(obj) for src, obj in objects.items() if src not in exclude),
                             "-o", str(binary), *LIBRARIES],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


def run_test(binary, out, script="skipboot\nroot home\nwait 50\n"):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "build").mkdir(exist_ok=True)
    boot = out / "boot.txt"
    boot.write_text(script)
    return subprocess.run([str(binary), "--script", str(boot), "--out", str(out / "frames"),
                           "--no-media", "--quiet"], cwd=out,
                          capture_output=True, text=True, timeout=60)
