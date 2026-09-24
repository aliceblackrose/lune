# Frame-owned captures: second optimization pass

Measured 2026-09-24 against the already optimized revision
`c720ef0dacc76c08860fd5057e25f6947b7301de`, on the same x86-64 AMD EPYC 9V74
machine with GCC 13.3.0 and the default C17 `-O2` warning flags.

## Production source latency

Seven alternating pairs, reversing binary order on each pair, after one warmup
per binary/source. Wall time includes launching the production CLI and checking
the source through the self-hosted compiler. Both executables use identical
stage bytecode. Raw samples are in `closure-optimization-results.json`.

| Source checked | Previous branch | This change | Latency reduction |
| --- | ---: | ---: | ---: |
| compiler/lexer.lune | 94.022 ms | 31.371 ms | 66.6% |
| compiler/parser.lune | 178.830 ms | 56.518 ms | 68.4% |
| compiler/compiler.lune | 315.341 ms | 104.422 ms | 66.9% |
| compiler/formatter.lune | 83.667 ms | 28.332 ms | 66.1% |

The separate standard 30-sample startup probe measured 3.253 ms before and
2.128 ms after (35% lower). This is source-script startup, including compilation,
not bare VM initialization. These timings are machine/workload-specific.

## Runtime comparison

Seven alternating pairs with identical updated benchmark source linked against
each runtime. These are medians of each process's per-workload averages.

| Workload | Previous branch | This change |
| --- | ---: | ---: |
| frontend parse | 103.100 MiB/s | 101.320 MiB/s |
| bytecode compile | 2.428 ms | 2.536 ms |
| integer loop | 61.392 ms | 60.201 ms |
| function calls | 10.562 ms | 10.255 ms |
| list indexing | 32.305 ms | 32.293 ms |
| map lookup | 28.528 ms | 28.530 ms |
| string workload | 0.576 ms | 0.582 ms |
| GC-heavy allocation | 8.825 ms | 9.103 ms |
| large map lookup | 37.941 ms | 37.427 ms |
| local integer loop | 40.372 ms | 39.367 ms |
| calls with outer capture | 10.119 ms | 6.243 ms |

The targeted captured-variable workload improves by 38%. Other workloads vary
by roughly 0–4%; no improvement is claimed for those small differences. Native
frontend parse/compile timing is unaffected by the changed closure algorithm.

## Why this helps

A short `gprof` run checking `compiler/parser.lune` attributed 11 of its 14
10-ms samples to `close_frame_upvalues`. The previous implementation scanned
all open captured variables on every function return. For each captured variable
it compared its location against up to 256 slots to discover the owning frame.
The self-hosted compiler makes many calls while outer captures stay open.

Each call frame now owns a list of its open captures. Capture deduplication
searches that frame's list, GC marks captures through active frames, and return
closes only that frame's list. A call with no captures has constant-time capture
cleanup even when outer frames hold many captures. Returning from a frame with
k captures takes O(k), rather than scanning captures from unrelated frames.

This removes the membership scan and the VM-global list. It adds one pointer
per call frame (and removes one VM-global pointer), with no extra per-capture
allocation. The bytecode format and language behavior are unchanged.

## Validation

- `make test`: lexer/parser, syntax, examples, self-host tests, byte-for-byte
  stage-1/stage-2 images, production CLI, and new REPL error-recovery checks pass.
- VM tests run with GC stress enabled. New tests cover an outer capture remaining
  live across inner returns and multiple captured callback frames reusing slots.
  Existing shared/mutating/nested captures and local recursion still pass.
- Only the pre-existing `exec-capture` check fails, as before: this environment
  has no `/tmp`, so libc `tmpfile()` cannot create its capture files.
- AddressSanitizer/UndefinedBehaviorSanitizer report no memory/UB errors in the VM
  suite, with that same known test failure. Leak checking remains disabled because
  LeakSanitizer does not support this environment's ptrace setup.

## Reproduce

Keep each production executable beside its matching `selfhost/` directory.
Build with the same compiler and flags, then run:

```sh
python3 bench/production.py --baseline /path/to/previous/lune
python3 bench/production.py --baseline /path/to/previous/lune --json
python3 bench/startup.py
make bench
make test
```

Set `TMPDIR` to an existing writable directory if the environment lacks a default
temporary directory. For runtime comparison, build this branch's `bench/bench.c`
against both revisions so the added captured-variable workload is identical.
