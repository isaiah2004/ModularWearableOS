"""Filesystem and subprocess helpers for host jobs (no import-time mutations)."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    # Readers/AV can briefly deny replacement on Windows. Retry the atomic
    # rename only: the complete new JSON is already durable and closed.
    deadline = time.monotonic() + 1.0
    delay = 0.01
    for attempt in range(32):
        try:
            os.replace(temporary, path)
            return
        except OSError as exc:
            if getattr(exc, 'winerror', None) not in (5, 32, 33):
                raise
            remaining = deadline - time.monotonic()
            if remaining <= 0 or attempt == 31:
                raise
            time.sleep(min(delay, remaining))
            delay = min(0.05, delay * 2)


def contained(root, path):
    root, path = Path(root).resolve(), Path(path).resolve()
    if path == root or root not in path.parents:
        raise ValueError(f"Output must be a child of {root}: {path}")
    return path


def local_environment(root=ROOT, *, minimum_free=0):
    """Use the workspace volume for heavy TEMP/TMP; preserve all existing dirs."""
    root = Path(root).resolve()
    temporary = contained(root, root / ".worktrees/_audit_tmp")
    temporary.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(temporary).free
    if free < minimum_free:
        raise OSError(f"{temporary.drive or temporary}: needs {minimum_free} free bytes; has {free}")
    environment = os.environ.copy()
    environment.update(TEMP=str(temporary), TMP=str(temporary), TMPDIR=str(temporary))
    return environment


def run_directory(prefix, root=ROOT, *, parent=None, minimum_free=0):
    environment = local_environment(root, minimum_free=minimum_free)
    directory = Path(parent or environment["TEMP"])
    contained(root, directory)
    directory.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=prefix + "-", dir=directory))


def checked_run(command, *, cwd=ROOT, env=None, log=None, **kwargs):
    result = subprocess.run([str(value) for value in command], cwd=cwd, env=env,
                            capture_output=True, text=True, **kwargs)
    output = result.stdout + result.stderr
    if log:
        Path(log).write_text(output, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Command exited {result.returncode}: {command[0]}\n{output[-12000:]}")
    return result
