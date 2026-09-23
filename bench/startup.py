#!/usr/bin/env python3
"""Measure fresh-process startup for the production Lune executable."""

from __future__ import annotations

import pathlib
import statistics
import subprocess
import sys
import time


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    binary = root / "bootstrap" / "build" / "lune"
    script = root / "examples" / "scalars.lune"

    if len(sys.argv) > 1:
        binary = pathlib.Path(sys.argv[1])

    samples: list[float] = []

    for _ in range(5):
        subprocess.run(
            [str(binary), "run", str(script)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )

    for _ in range(30):
        start = time.perf_counter()
        subprocess.run(
            [str(binary), "run", str(script)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        samples.append((time.perf_counter() - start) * 1000.0)

    print(
        f"{'process startup':24} "
        f"{statistics.median(samples):10.3f} ms median"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
