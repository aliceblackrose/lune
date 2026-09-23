# Lune 0.1 performance baseline

This file records a reference measurement for regression tracking. It is not a portable performance guarantee.

## Reference run

- Date: 2026-09-23
- Revision: `c7a63611bb0663d0987ea7f52ec923283fbe19a6`
- GitHub Actions run: `35848258232`
- Runner: `ubuntu-latest`
- Build flags: `-std=c17 -O2 -Wall -Wextra -Wpedantic -Werror`
- Command: `make bench`

Results:

| Benchmark | 0.1 baseline |
| --- | ---: |
| Frontend parse | 160.97 MiB/s |
| Bytecode compile | 1.511 ms/run |
| Integer loop | 78.878 ms/run |
| Function calls | 11.361 ms/run |
| List indexing | 44.368 ms/run |
| Map lookup | 39.148 ms/run |
| String workload | 0.592 ms/run |
| GC-heavy allocation | 8.174 ms/run |
| Fresh-process startup | 2.770 ms median |

## Interpretation

Hosted CI hardware varies between runs, so these values should be treated as an order-of-magnitude reference. For optimization work:

1. compare on the same machine and toolchain;
2. run `make bench` several times;
3. compare medians rather than a single sample;
4. investigate sustained regressions, not ordinary scheduler noise.

The benchmark harness exists to prevent accidental performance drift and to justify runtime complexity with measured wins. It is not a substitute for representative Lune application benchmarks.
