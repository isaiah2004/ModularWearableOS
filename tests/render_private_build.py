"""Fresh current-source simulator objects for rendering integration tests.

The process-local build is shared by wrappers in the same unittest run. Every
new process compiles all sources again; no objects from another build are used.
All compiler temporary files, fixtures and simulator writes stay in CHECKS.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ROOT / "build/alpha05/checks/engine/render-tests"
FLAGS = ["-O2", "-std=c11", "-DNOS_PSRAM_TRACKING", "-DGFX_COST_MODEL",
         *("-I" + str(ROOT / path) for path in ("core", "nos", "nos/fonts", "sim", "firmware/main")),
         "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-format-truncation", "-Werror"]
LIBRARIES = ["-lgdi32", "-luser32", "-lwinmm", "-lm"]
AVAILABLE = os.name == "nt" and bool(shutil.which("gcc"))
_objects = None
_identity = None
_lock = threading.Lock()


def workspace(prefix):
    CHECKS.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=prefix + "-", dir=CHECKS))


def stage_sky(out):
    """Copy the tracked belt/moons catalogs into the run's private SD root."""
    sky = Path(out).resolve() / "sd" / "sky"
    sky.mkdir(parents=True, exist_ok=True)
    for name in ("belt.bin", "moons.bin"):
        shutil.copy(ROOT / "assets/sd/sky" / name, sky / name)


def environment(out, **extra):
    out = Path(out).resolve()
    assert out.is_relative_to(CHECKS.resolve()), out
    out.mkdir(parents=True, exist_ok=True)
    sd = out / "sd"
    sd.mkdir(exist_ok=True)
    return os.environ | {key: str(out) for key in ("TEMP", "TMP", "TMPDIR")} | {
        "NOS_SD_ROOT": str(sd)} | extra


def _inputs():
    core = ("asset_job", "gfx", "font", "sprite", "anim", "notif", "battery",
            "pedometer", "walk", "motion_wake", "wake_service", "knock", "ephem")
    sources = sorted((ROOT / "nos").rglob("*.c"))
    sources += [ROOT / "core" / (name + ".c") for name in core]
    sources += sorted((ROOT / "core/fonts").glob("*.c"))
    sources += sorted((ROOT / "sim").glob("nos_*.c"))
    # Headers/includes are part of the identity too: a stale structure layout is
    # unsafe even when its corresponding .c timestamp has not changed.
    dependencies = set(sources)
    for folder in ("core", "nos", "sim", "firmware/main"):
        dependencies.update(path for path in (ROOT / folder).rglob("*")
                            if path.suffix in (".h", ".inc"))
    hashes = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted(dependencies)}
    return sources, hashes


def objects():
    global _objects, _identity
    with _lock:
        sources, identity = _inputs()
        if _objects is not None and identity == _identity:
            return dict(_objects)
        out = workspace("objects")
        env = environment(out)

        def compile_source(source):
            relative = source.relative_to(ROOT).as_posix()
            obj = out / (relative.replace("/", "_").replace(".c", ".o"))
            extra = ["-Dmain=sim_program_main"] if relative == "sim/nos_sim.c" else []
            result = subprocess.run(["gcc", *FLAGS, *extra, "-c", str(source), "-o", str(obj)],
                                    cwd=out, env=env, capture_output=True, text=True)
            return relative, obj, result

        with ThreadPoolExecutor(max_workers=6) as pool:
            results = list(pool.map(compile_source, sources))
        log = "\n".join(f"{path}: {run.returncode}\n{run.stdout}{run.stderr}"
                        for path, _, run in results)
        (out / "compile.log").write_text(log)
        (out / "inputs.json").write_text(json.dumps({"flags": FLAGS, "sha256": identity}, indent=2) + "\n")
        failures = [path for path, _, run in results if run.returncode]
        if failures:
            raise AssertionError("Fresh render compilation failed: " + ", ".join(failures) + "\n" + log)
        if _inputs()[1] != identity:
            raise AssertionError("Source/header changed during render test build; rerun with stable sources")
        _objects = {path: obj for path, obj, _ in results}
        _identity = identity
        return dict(_objects)


def link(source, binary, exclude=(), defines=(), extra_flags=()):
    binary = Path(binary).resolve()
    env = environment(binary.parent)
    source = Path(source)
    if not source.is_absolute():
        source = ROOT / source
    current = objects()
    unknown = set(exclude) - current.keys()
    assert not unknown, unknown
    result = subprocess.run(["gcc", *FLAGS, *extra_flags,
                             *("-D" + define for define in defines), str(source),
                             *(str(obj) for path, obj in current.items() if path not in exclude),
                             "-o", str(binary), *LIBRARIES], cwd=binary.parent, env=env,
                            capture_output=True, text=True)
    binary.with_suffix(".link.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


def simulator(binary):
    source = Path(binary).with_suffix(".main.c")
    source.write_text("extern int sim_program_main(int,char **);\n"
                      "int main(int argc,char **argv){return sim_program_main(argc,argv);}\n")
    link(source, binary)


def run(binary, out, script="wait 25\n", args=(), timeout=90, env=None):
    out = Path(out).resolve()
    runtime_env = environment(out) | (env or {})
    (out / "build").mkdir(exist_ok=True)
    boot = out / "boot.txt"
    boot.write_text(script)
    result = subprocess.run([str(binary), "--script", str(boot), "--out", str(out / "frames"),
                             "--no-media", "--quiet", *args], cwd=out, env=runtime_env,
                            capture_output=True, text=True, timeout=timeout)
    (out / "run.log").write_text(result.stdout + result.stderr)
    return result
