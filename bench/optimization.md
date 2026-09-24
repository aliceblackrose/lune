# Runtime optimization measurements

Measured on 2026-09-24, x86-64 AMD EPYC 9V74, GCC 13.3.0, with the default
`-std=c17 -O2 -Wall -Wextra -Wpedantic -Werror` flags. Baseline revision:
`1b2123280f47c94ac227e038a70fb12262e459c3`.

These are the best retained variants from this optimization pass, not a claim
of a universal minimum execution time. Hardware, workloads, and scheduler noise
change results. The earlier CI baseline uses different hardware and is not a
valid before/after comparison.

## Results

Seven alternating baseline/optimized process pairs, reversing execution order
on each pair. Both versions use the updated benchmark harness, including result
validation and identical iteration counts. Entries below are medians of the
seven per-process averages. Raw samples are in `optimization-results.json`.

| Benchmark | Baseline | Optimized | Change |
| --- | ---: | ---: | ---: |
| frontend parse | 106.100 MiB/s | 103.350 MiB/s | -2.6% |
| bytecode compile | 2.559 ms | 2.407 ms | -5.9% |
| integer loop | 133.452 ms | 61.576 ms | -53.9% |
| function calls | 18.513 ms | 10.368 ms | -44.0% |
| list indexing | 78.905 ms | 32.050 ms | -59.4% |
| map lookup | 70.533 ms | 29.137 ms | -58.7% |
| string workload | 0.929 ms | 0.585 ms | -37.0% |
| GC-heavy allocation | 14.315 ms | 9.158 ms | -36.0% |
| large map lookup | 283.268 ms | 38.211 ms | -86.5% |
| local integer loop | 57.865 ms | 40.813 ms | -29.5% |
| source check | 182.928 ms | 174.974 ms | -4.3% |
| startup | 4.134 ms | 3.606 ms | -12.8% |

Higher is better for parse throughput; lower is better for all timing rows.
`source check` is wall time for the production CLI's `check compiler/parser.lune`;
`startup` is wall time for `run examples/scalars.lune`. These two rows include
process launch and use one sample per pair. Treat their smaller changes, and
frontend changes, as inconclusive without more measurements. The standalone
30-sample `bench/startup.py` probe measured 3.283 ms before and 3.220 ms after,
also a small difference.

## Changes and experiments

- Inline trivial value constructors, truthiness checks, and object-kind checks,
  allowing the C compiler to remove cross-translation-unit calls without LTO.
- Retain linear lookup for maps with at most eight entries; larger maps use an
  open-addressed index at no more than 50% load. Entries remain contiguous and
  insertion-ordered, preserving printing, JSON serialization, and GC traversal.
  Index allocation is accounted for and freed with the map. This costs an
  additional pointer and capacity field per map and an index allocation for larger maps.
- Update existing globals in one lookup; missing assignments still fail without
  inserting a new binding.
- Assert benchmark results and add large-map and local-variable loop coverage.

Initial five-run experiments reduced the original integer-loop median from
130.278 ms to 116.751 ms with inline value helpers, then 67.981 ms with indexed
maps, 65.258 ms with single-lookup updates, and 62.545 ms with inline object
predicates. These sequential experiments guided selection; the interleaved
comparison above is the final evidence.

A four-entry linear/hash transition was also tested. Five alternating production
parser-source checks measured 185.390 ms with that threshold versus 183.443 ms
with eight entries. The earlier transition was rejected because it did not show
a benefit and allocated indexes for more small maps. No compiler flags, bytecode
format, or language semantics were changed.

## Validation

`make test` passes lexer/parser, syntax, examples, self-host compiler/formatter,
byte-for-byte stage-1/stage-2 image comparisons, and production CLI checks.
All VM checks pass except the pre-existing `exec-capture` test, which fails on
both baseline and optimized revisions in this environment: libc `tmpfile()`
cannot create stdout capture because `/tmp` is unavailable. A workspace
`TMPDIR` was set for the production compiler's temporary files.

New GC-stress tests exercise 300-key map growth and replacement, retained list
values, missing/empty/embedded-NUL keys, and insertion-order serialization.
AddressSanitizer and UndefinedBehaviorSanitizer reported no memory/UB errors
while running the VM suite (with the same `exec-capture` failure). LeakSanitizer
was disabled after reporting that this execution environment uses ptrace, which
it does not support; leak checking is therefore not claimed.

## Reproduction

Build both revisions with the same compiler and flags. For the baseline, compile
this branch's `bench/bench.c` against the baseline C sources so the workload and
result checks are identical. Run each benchmark executable seven times, alternating
which version runs first, and compare medians. The in-process harness performs
one warmup and five timed iterations per workload, or three for the string/GC
workloads. Run `make test` and `python3 bench/startup.py` separately from timings.
