#!/usr/bin/env python3
"""Measure production source compilation with optional interleaved comparison."""

from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import subprocess
import time


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path,
                        default=root / "bootstrap/build/lune")
    parser.add_argument("--baseline", type=pathlib.Path)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("--samples must be positive")

    binaries = {"optimized": args.binary.resolve()}
    if args.baseline is not None:
        binaries = {"baseline": args.baseline.resolve(), **binaries}
    sources = ["compiler/lexer.lune", "compiler/parser.lune",
               "compiler/compiler.lune", "compiler/formatter.lune"]
    results: dict[str, dict[str, list[float]]] = {
        name: {source: [] for source in sources} for name in binaries
    }

    def measure(binary: pathlib.Path, source: str) -> float:
        start = time.perf_counter()
        subprocess.run([str(binary), "check", str(root / source)],
                       cwd=root, check=True, stdout=subprocess.DEVNULL)
        return (time.perf_counter() - start) * 1000

    for binary in binaries.values():
        for source in sources:
            measure(binary, source)
    for sample in range(args.samples):
        names = list(binaries)
        if sample % 2:
            names.reverse()
        for source in sources:
            for name in names:
                results[name][source].append(measure(binaries[name], source))

    if args.json:
        print(json.dumps({"binaries": {k: str(v) for k, v in binaries.items()},
                          "unit": "ms wall time", "samples": results}, indent=2))
    else:
        for source in sources:
            times = "  ".join(
                f"{name}: {statistics.median(results[name][source]):.3f} ms"
                for name in binaries
            )
            print(f"{source:26} {times}")


if __name__ == "__main__":
    main()
