# Lune benchmarks

The benchmark suite is a baseline for finding regressions and deciding which runtime optimizations are worth adding. It is not intended to produce portable absolute scores across different machines. The recorded Lune 0.1 CI reference is in [baseline.md](baseline.md).

Run:

```sh
make bench
```

The suite measures:

- fresh-process startup;
- lexer/parser throughput;
- bytecode compilation;
- a tight integer loop;
- function-call overhead;
- list indexing;
- map member lookup;
- string concatenation/allocation;
- GC-heavy collection allocation.

The in-process harness uses the C standard library `clock()` so it remains dependency-free. The startup probe uses Python 3 only as benchmark tooling and is not a Lune runtime dependency.

For meaningful comparisons, use the same machine, compiler, optimization flags, and revision/build configuration. Run the suite several times and compare medians rather than a single sample.

Do not optimize a subsystem solely because a microbenchmark exists. The benchmark should identify a measurable bottleneck in representative scripts before the VM grows more complex.

The measured runtime optimization pass, tradeoffs, and validation limits are
recorded in [optimization.md](optimization.md). VM workloads validate their result
on every run and include large-map lookup and a local-variable integer loop.
