#!/usr/bin/env python3
"""Every source-derived test executable must be ignored, never its source."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "tools" / "cpp"

sources = list(CPP.glob("test_*.cpp"))
sources += list((CPP / "gpu").glob("*_test.cpp"))
sources += list((CPP / "gpu").glob("*_smoke.cpp"))
sources += list((CPP / "qdsp5").glob("*_test.cpp"))

def ignored(path: Path) -> bool:
    rel = path.relative_to(ROOT)
    return subprocess.run(["git", "check-ignore", "-q", str(rel)], cwd=ROOT).returncode == 0

failures = []
for src in sources:
    binary = src.with_suffix("")
    if ignored(src):
        failures.append(f"source is ignored: {src.relative_to(ROOT)}")
    if not ignored(binary):
        failures.append(f"binary is not ignored: {binary.relative_to(ROOT)}")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    print(f"binary-ignore hygiene RED: {len(failures)} failure(s)")
    sys.exit(1)
print(f"binary-ignore hygiene GREEN: {len(sources)} source/binary pairs covered")
